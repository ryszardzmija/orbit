#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include "reactor_fixture.h"

namespace orbit::proxy {

using namespace std::chrono_literals;

TEST_F(ProxyReactorTest, ConfiguredWatermarksLimitReadsAndPreserveAPartialSendSuffix) {
    struct Io {
        std::vector<size_t> receive_sizes;
        int sends = 0;
    } io;
    createReactor({}, {},
                  {
                      .context = &io,
                      .receive =
                          [](void* context, int fd, void* data, size_t size, int flags) {
                              static_cast<Io*>(context)->receive_sizes.push_back(size);
                              return recv(fd, data, size, flags);
                          },
                      .send =
                          [](void* context, int fd, const void* data, size_t size, int flags) {
                              int attempt = ++static_cast<Io*>(context)->sends;
                              if (attempt == 1) {
                                  size = 2;
                              } else if (attempt == 2) {
                                  errno = EAGAIN;
                                  return ssize_t{-1};
                              }
                              return send(fd, data, size, flags);
                          },
                  },
                  {.buffer_high_watermark = 4099, .buffer_low_watermark = 1});
    auto source = test::socketPair();
    auto dest = test::socketPair();
    auto ids = install(source, dest);
    std::string payload;
    for (size_t i = 0; i < 4110; ++i) {
        payload += static_cast<char>('a' + i % 26);
    }
    ASSERT_EQ(ssize_t(payload.size()), send(source.peer.get(), payload.data(), payload.size(), 0));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_FALSE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_EQ((std::vector<size_t>{4096, 5}), io.receive_sizes);
    EXPECT_EQ(1, io.sends);

    // A blocked write and then a 4096-byte write leave the queue above its configured low mark.
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_FALSE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_FALSE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_TRUE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));

    std::string received(payload.size(), '\0');
    ASSERT_EQ(ssize_t(received.size()), recv(dest.peer.get(), received.data(), received.size(), 0));
    EXPECT_EQ(payload, received);
}

TEST_F(ProxyReactorTest, RetiredSourceCannotDispatchToAReusedDescriptor) {
    auto downstream = test::socketPair();
    auto upstream = test::socketPair();
    int old_fd = downstream.local.get();
    auto old = install(downstream, upstream);
    detail::ReadyEvent stale{old.downstream, EPOLLIN | EPOLLERR | EPOLLHUP};
    ASSERT_TRUE(dispatch({old.upstream, EPOLLERR | EPOLLIN | EPOLLOUT}));
    ASSERT_FALSE(registered(old.downstream));
    ASSERT_FALSE(registered(old.upstream));

    auto new_downstream = test::socketPair();
    auto new_upstream = test::socketPair();
    ASSERT_EQ(old_fd, new_downstream.local.get());
    auto live = install(new_downstream, new_upstream);
    EXPECT_NE(old.downstream, live.downstream);
    ASSERT_EQ(1, send(new_downstream.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch(stale));
    char byte;
    ASSERT_EQ(-1, recv(new_upstream.peer.get(), &byte, 1, 0));
    EXPECT_EQ(EAGAIN, errno);
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    ASSERT_EQ(1, recv(new_upstream.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);
}

TEST_F(ProxyReactorTest, RetiredEndpointIgnoresLateErrorWhilePeerStillHasOutput) {
    auto source = test::socketPair();
    auto dest = test::socketPair();
    auto ids = install(source, dest);
    ASSERT_EQ(1, send(source.peer.get(), "q", 1, 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_RDWR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    ASSERT_FALSE(registered(ids.downstream));
    ASSERT_TRUE(registered(ids.upstream));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLERR | EPOLLOUT}));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    char byte;
    ASSERT_EQ(1, recv(dest.peer.get(), &byte, 1, 0));
    EXPECT_EQ('q', byte);
    EXPECT_FALSE(registered(ids.upstream));
}

TEST_F(ProxyReactorTest, StaleReadCannotInterruptSurvivingDirection) {
    auto source = test::socketPair();
    auto dest = test::socketPair();
    int dest_fd = dest.local.get();
    auto ids = install(source, dest);
    ASSERT_EQ(1, send(source.peer.get(), "a", 1, 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_RDWR));
    ASSERT_EQ(1, send(dest.peer.get(), "b", 1, 0));

    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP | EPOLLIN | EPOLLOUT}));
    ASSERT_TRUE(registered(ids.upstream));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN | EPOLLRDHUP}));
    char byte;
    ASSERT_EQ(1, recv(dest_fd, &byte, 1, MSG_PEEK));
    EXPECT_EQ('b', byte);

    // The final queued write and EOF close the session during this notification.
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT | EPOLLRDHUP}));
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLHUP}));
    ASSERT_EQ(1, recv(dest.peer.get(), &byte, 1, 0));
    EXPECT_EQ('a', byte);
}

TEST_F(ProxyReactorTest, EofAlonePropagatesFinAndKeepsReverseStreamOpen) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    char byte;
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    ASSERT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    ASSERT_EQ(1, send(backend.peer.get(), "r", 1, 0));
    ASSERT_EQ(0, shutdown(backend.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN}));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('r', byte);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN}));
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_FALSE(registered(ids.downstream));
}

TEST_F(ProxyReactorTest, PendingOnlyShutdownInvalidatesCompletionAndClosesBothSockets) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    auto id = installPending(client, attempt);
    auto second_client = test::socketPair();
    auto second_attempt = test::socketPair();
    auto second = installPending(second_client, second_attempt);
    auto listener = listenerId();
    ASSERT_TRUE(shutdownRequest());
    EXPECT_FALSE(registered(listener));
    EXPECT_FALSE(registered(id));
    EXPECT_FALSE(registered(second));
    EXPECT_TRUE(stopped());
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    ASSERT_TRUE(dispatch({id, EPOLLOUT}));
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(second_client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(second_attempt.peer.get(), &byte, 1, 0));
}

TEST_F(ProxyReactorTest, GracefulShutdownCancelsPendingsAndTimerEscalatesEstablishedSession) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    auto waiting = test::socketPair();
    auto attempt = test::socketPair();
    auto pending = installPending(waiting, attempt);
    ASSERT_TRUE(shutdownRequest());
    ASSERT_TRUE(handleTimer()); // Copied timer readiness without an expiration.
    EXPECT_FALSE(stopped());
    EXPECT_FALSE(registered(pending));
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    ASSERT_EQ(1, send(backend.peer.get(), "g", 1, 0));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN}));
    char byte;
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('g', byte);
    // Expire the real one-shot timer without waiting 30 seconds.
    shortenGrace();
    ASSERT_TRUE(arm());
    pollfd timer{timerFd(), POLLIN, 0};
    ASSERT_EQ(1, poll(&timer, 1, 1000));
    ASSERT_TRUE(handleTimer());
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
}

TEST_F(ControlledReactorTest, ExpiredBudgetBeatsAlreadyWritableCompletion) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    const auto at = clock.now() + 10ms;
    auto pending = installPending(client, attempt, at);
    ASSERT_EQ(at, nextDeadline());
    clock.advanceTo(at);
    // SO_ERROR on this connected socket is zero. Its copied completion precedes the timer.
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_FALSE(registered(pending));
    EXPECT_FALSE(watching(pending));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
}

TEST_F(ControlledReactorTest, SuccessfulPromotionCancelsDialDeadlineAndRetainsHalfClosedRequest) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    ASSERT_EQ(1, send(client.peer.get(), "q", 1, 0));
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    const auto at = clock.now() + 10ms;
    auto pending = installPending(client, attempt, at);
    ASSERT_EQ(at, nextDeadline());
    clock.advanceTo(at - 1ns);
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(pending));
    clock.advanceTo(at);
    ASSERT_TRUE(expire(clock.now()));
    auto live = currentSessionSources();
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    char byte;
    ASSERT_EQ(1, recv(attempt.peer.get(), &byte, 1, 0));
    EXPECT_EQ('q', byte);
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
    ASSERT_EQ(1, send(attempt.peer.get(), "r", 1, 0));
    ASSERT_TRUE(dispatch({live.upstream, EPOLLIN}));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('r', byte);
}

TEST_F(ProxyReactorTest, StalledAttemptExpiresWithoutDisturbingEstablishedTraffic) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto live = install(client, backend);
    auto waiting = test::socketPair();
    auto attempt = test::socketPair();
    auto id = installPending(waiting, attempt, detail::DeadlineClock::now() + 2ms);
    ASSERT_TRUE(arm());
    pollfd timer{timerFd(), POLLIN, 0};
    ASSERT_EQ(1, poll(&timer, 1, 1000));
    ASSERT_TRUE(handleTimer());
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(id));
    ASSERT_TRUE(dispatch({id, EPOLLOUT}));
    char byte;
    EXPECT_EQ(0, recv(waiting.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
    ASSERT_EQ(1, send(client.peer.get(), "a", 1, 0));
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('a', byte);
}

class ConnectedHandoffDeadlineTest : public ControlledReactorTest,
                                     public testing::WithParamInterface<std::chrono::nanoseconds> {
protected:
    void SetUp() override {
        createReactor({.idle = 60s},
                      {this, [](void* context) noexcept {
                           auto& fixture = *static_cast<ConnectedHandoffDeadlineTest*>(context);
                           const auto now = fixture.clock.source().now();
                           if (now == fixture.handoff_time) {
                               const auto [downstream, upstream] = fixture.handoff_sources;
                               fixture.observed_both_endpoints =
                                   fixture.registered(downstream) && fixture.watching(downstream) &&
                                   fixture.registered(upstream) && fixture.watching(upstream);
                           }
                           return now;
                       }});
    }

    std::optional<detail::Deadline> handoff_time;
    detail::SessionSourceIds handoff_sources{};
    bool observed_both_endpoints = false;
};

TEST_P(ConnectedHandoffDeadlineTest, ExpiredHandoffReleasesBothRegisteredEndpoints) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    const int client_fd = client.local.get();
    const int backend_fd = backend.local.get();
    const auto at = clock.now() + 1ms;
    const auto pending = installPending(client, backend, at);
    ASSERT_EQ(at, nextDeadline());
    ASSERT_TRUE(watching(pending));
    // The handoff allocates the next two source IDs for the connected endpoints.
    handoff_sources = {pending + 1, pending + 2};
    handoff_time = at + GetParam();
    clock.advanceTo(at - 1ns);
    clock.advanceAfterNextRead(*handoff_time);

    // Completion reads just before expiry. The handoff reads after both watches are added.
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_TRUE(observed_both_endpoints);
    for (auto source : {pending, handoff_sources.downstream, handoff_sources.upstream}) {
        EXPECT_FALSE(registered(source)) << source;
        EXPECT_FALSE(watching(source)) << source;
    }
    EXPECT_FALSE(nextDeadline());
    for (int fd : {client_fd, backend_fd}) {
        EXPECT_EQ(-1, fcntl(fd, F_GETFD)) << fd;
        EXPECT_EQ(EBADF, errno) << fd;
    }
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    ASSERT_TRUE(shutdownRequest());
    EXPECT_TRUE(stopped());
}

INSTANTIATE_TEST_SUITE_P(ConnectDeadline, ConnectedHandoffDeadlineTest, testing::Values(0ns, 1ns),
                         [](const testing::TestParamInfo<std::chrono::nanoseconds>& info) {
                             return info.param == 0ns ? "AtDeadline" : "AfterDeadline";
                         });

class IdleHandoffTest : public ControlledReactorTest {
protected:
    void SetUp() override {
        createReactor(
            {.idle = 1ms}, clock.source(), {}, {.max_connections = 1, .max_pending_connections = 1},
            {.allocation = {this, [](void* context, detail::AllocationPoint point) noexcept {
                                auto& self = *static_cast<IdleHandoffTest*>(context);
                                if (point == detail::AllocationPoint::DeadlineIndex) {
                                    if (auto at =
                                            std::exchange(self.preparation_ends, std::nullopt)) {
                                        ++self.observed_preparations;
                                        EXPECT_EQ(1u, self.pendingCount());
                                        EXPECT_EQ(1u, self.totalCount());
                                        EXPECT_EQ(0u, self.sessionCount());
                                        EXPECT_FALSE(self.canAdmit());
                                        self.clock.advanceTo(*at);
                                    }
                                }
                                return false;
                            }}});
    }

    std::optional<detail::Deadline> preparation_ends;
    int observed_preparations = 0;
};

TEST_F(IdleHandoffTest, CompleteIdleIntervalStartsAfterEstablishmentPreparation) {
    int preparations = 0;
    for (auto delay : {1ms, 3ms}) {
        SCOPED_TRACE(delay.count());
        auto client = test::socketPair();
        auto upstream = test::socketPair();
        auto pending = installPending(client, upstream, clock.now() + 30ms);
        const auto handoff = clock.now() + delay;
        const auto expires = handoff + 1ms;
        preparation_ends = handoff;
        ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
        EXPECT_EQ(++preparations, observed_preparations);
        ASSERT_EQ(expires, nextDeadline());
        EXPECT_EQ(0u, pendingCount());
        EXPECT_EQ(1u, totalCount());
        const auto live = currentSessionSources();
        // I/O at handoff leaves this deadline unchanged, since the controlled clock is stationary.
        ASSERT_EQ(1, send(client.peer.get(), "a", 1, 0));
        ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
        char byte;
        ASSERT_EQ(1, recv(upstream.peer.get(), &byte, 1, 0));
        EXPECT_EQ('a', byte);
        EXPECT_EQ(expires, nextDeadline());
        clock.advanceTo(expires - 1ns);
        ASSERT_TRUE(expire(clock.now()));
        EXPECT_TRUE(registered(live.downstream));
        clock.advanceTo(expires);
        ASSERT_TRUE(expire(clock.now()));
        EXPECT_FALSE(registered(live.downstream));
        EXPECT_FALSE(registered(live.upstream));
        EXPECT_EQ(0u, totalCount());
        EXPECT_FALSE(nextDeadline());
        EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
        EXPECT_EQ(0, recv(upstream.peer.get(), &byte, 1, 0));
    }
}

TEST_F(IdleHandoffTest, PreparationReachingConnectDeadlineRollsBackTheIdleKeyAndWatches) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto fds = {client.local.get(), upstream.local.get()};
    const auto deadline = clock.now() + 1ms;
    auto pending = installPending(client, upstream, deadline);
    preparation_ends = deadline;
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_EQ(1, observed_preparations);
    EXPECT_EQ(deadline, clock.now());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(0u, sessionCount());
    EXPECT_EQ(0u, totalCount());
    EXPECT_FALSE(nextWakeup());
    EXPECT_TRUE(listenerAccepting());
    for (auto id : {pending, pending + 1, pending + 2}) {
        EXPECT_FALSE(registered(id));
        EXPECT_FALSE(watching(id));
    }
    for (int fd : fds) {
        EXPECT_EQ(-1, fcntl(fd, F_GETFD));
        EXPECT_EQ(EBADF, errno);
    }
}

TEST_F(ControlledReactorTest, ExpiredPendingHandoffReleasesBothSockets) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    const auto at = clock.now() + 1ms;
    clock.advanceTo(at);
    ASSERT_TRUE(dialResult(detail::UpstreamDialInProgress{
        {std::move(client.local), {}, std::move(attempt.local), 0, at}}));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
}

TEST_F(ControlledReactorTest, ExpiredPendingHandoffKeepsListenerEnabled) {
    createReactor({}, clock.source(), {}, {.max_connections = 1, .max_pending_connections = 1});
    ASSERT_TRUE(listenerAccepting());
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    const auto deadline = clock.now() + 1ms;
    clock.advanceTo(deadline);

    ASSERT_TRUE(dialResult(detail::UpstreamDialInProgress{
        {std::move(client.local), {}, std::move(attempt.local), 0, deadline}}));

    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(0u, totalCount());
    EXPECT_TRUE(canAdmit());
    EXPECT_TRUE(listenerAccepting());
}

TEST_F(ControlledReactorTest, SimulatedDeadlineCannotArmKernelTimer) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    installPending(client, attempt, clock.now() + 1ms);
    ASSERT_TRUE(nextDeadline());
    auto result = arm();
    ASSERT_FALSE(result);
    EXPECT_EQ(std::errc::operation_not_supported, result.error());
    itimerspec timer{};
    ASSERT_EQ(0, timerfd_gettime(timerFd(), &timer));
    EXPECT_EQ(0, timer.it_value.tv_sec);
    EXPECT_EQ(0, timer.it_value.tv_nsec);
}

TEST_F(ProxyReactorTest, RegistrationFailureReleasesClientAndDialDeadline) {
    auto waiting = test::socketPair();
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto at = detail::DeadlineClock::now() + 10s;
    ASSERT_TRUE(dialResult(
        detail::UpstreamDialInProgress{{std::move(waiting.local), {}, FileDescriptor(fd), 0, at}}));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(waiting.peer.get(), &byte, 1, 0));
}

TEST_F(ProxyReactorTest, ExpirationBurstLeavesDueWorkArmedForAnotherTurn) {
    createReactor({}, {}, {}, {.max_pending_connections = 128});
    std::vector<detail::SourceId> ids;
    auto at = detail::DeadlineClock::now();
    for (int i = 0; i < 65; ++i) {
        auto waiting = test::socketPair();
        auto attempt = test::socketPair();
        ids.push_back(installPending(waiting, attempt, at));
    }
    ASSERT_TRUE(expire(at));
    ASSERT_EQ(1, std::ranges::count_if(ids, [&](auto id) { return registered(id); }));
    ASSERT_EQ(at, nextDeadline());
    ASSERT_TRUE(arm());
    pollfd timer{timerFd(), POLLIN, 0};
    ASSERT_EQ(1, poll(&timer, 1, 1000));
    ASSERT_TRUE(handleTimer());
    EXPECT_FALSE(nextDeadline());
    EXPECT_EQ(0, std::ranges::count_if(ids, [&](auto id) { return registered(id); }));
}

TEST_F(ControlledReactorTest, PromotionUsesTheSameTotalSlotUntilAnIdleSessionExpires) {
    createReactor({.idle = 60s}, clock.source(), {},
                  {.max_connections = 2, .max_pending_connections = 2});
    auto client = test::socketPair();
    auto backend = test::socketPair();
    const auto live = install(client, backend);
    auto pending_client = test::socketPair();
    auto pending_peer = test::socketPair();
    const auto pending = installPending(pending_client, pending_peer);
    EXPECT_EQ(1u, pendingCount());
    EXPECT_FALSE(listenerAccepting());

    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(2u, sessionCount());
    EXPECT_FALSE(listenerAccepting());
    ASSERT_EQ(1, send(backend.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch({live.upstream, EPOLLIN}));
    char byte;
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);

    clock.advanceTo(clock.now() + 1s);
    setIdleAt(live.downstream, clock.now());
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_EQ(1u, sessionCount());
    EXPECT_TRUE(listenerAccepting());
}

TEST_F(ControlledReactorTest, PendingLimitSuppressesLiveAndStaleListenerNotifications) {
    createReactor({}, clock.source(), {}, {.max_connections = 3, .max_pending_connections = 1});
    auto waiting = test::socketPair();
    auto attempt = test::socketPair();
    const auto at = clock.now() + 10ms;
    const auto pending = installPending(waiting, attempt, at);
    const auto listener = listenerId();
    ASSERT_FALSE(listenerAccepting());
    const auto incoming = connectToListener();
    auto client = test::socketPair();
    auto backend = test::socketPair();
    const auto live = install(client, backend);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    const auto events = waitForEvents();
    ASSERT_TRUE(events);
    EXPECT_TRUE(std::ranges::none_of(*events, [&](auto e) { return e.source_id == listener; }));
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    EXPECT_EQ(1u, pendingCount());
    EXPECT_EQ(at, nextDeadline());
    char byte;
    ASSERT_EQ(-1, recv(incoming.get(), &byte, 1, MSG_DONTWAIT));
    EXPECT_EQ(EAGAIN, errno);
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);

    clock.advanceTo(at);
    ASSERT_TRUE(expire(at));
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(registered(pending));
    EXPECT_TRUE(listenerAccepting());
    auto resumed = waitForEvents();
    ASSERT_TRUE(resumed);
    EXPECT_TRUE(std::ranges::any_of(*resumed, [&](auto e) { return e.source_id == listener; }));
    ASSERT_TRUE(shutdownRequest());
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    EXPECT_FALSE(listenerAccepting());
    EXPECT_FALSE(registered(listener));
    EXPECT_EQ(1u, sessionCount());
}

} // namespace orbit::proxy
