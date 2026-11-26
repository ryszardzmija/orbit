#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <utility>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "reactor_fixture.h"

namespace orbit::proxy {

using namespace std::chrono_literals;
using detail::DeadlineClock;

namespace {

struct IoObservation {
    int fd = -1;
    std::optional<detail::Deadline> advance_to;
    std::optional<detail::Deadline> advance_after_read;
    int calls = 0;
    ssize_t result = 0;
    int error = 0;
    detail::Deadline observed_at;

    void record(test::ControlledClock& clock, int actual_fd, ssize_t count, int syscall_errno) {
        if (fd != actual_fd) {
            return;
        }
        ++calls;
        result = count;
        error = count < 0 ? syscall_errno : 0;
        observed_at = clock.now();
        if (auto at = std::exchange(advance_to, std::nullopt)) {
            clock.advanceTo(*at);
        }
        if (auto at = std::exchange(advance_after_read, std::nullopt)) {
            clock.advanceAfterNextRead(*at);
        }
    }
};

struct StreamObservation {
    test::ControlledClock& clock;
    IoObservation received;
    IoObservation sent;

    net::StreamOperations operations() {
        return {
            .context = this,
            .receive =
                [](void* context, int fd, void* buffer, size_t size, int flags) {
                    auto& io = *static_cast<StreamObservation*>(context);
                    const auto result = ::recv(fd, buffer, size, flags);
                    const int error = errno;
                    io.received.record(io.clock, fd, result, error);
                    errno = error;
                    return result;
                },
            .send =
                [](void* context, int fd, const void* buffer, size_t size, int flags) {
                    auto& io = *static_cast<StreamObservation*>(context);
                    const auto result = ::send(fd, buffer, size, flags);
                    const int error = errno;
                    io.sent.record(io.clock, fd, result, error);
                    errno = error;
                    return result;
                },
        };
    }
};

size_t fillSendQueue(int fd) {
    int limit = 4096;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &limit, sizeof(limit)) < 0) {
        throw std::system_error(errno, std::system_category(), "setsockopt(SO_SNDBUF)");
    }
    std::array<char, 1024> filler{};
    size_t filled = 0;
    while (filled < 64 * 1024) {
        auto n = send(fd, filler.data(), filler.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EAGAIN) {
            return filled;
        }
        if (n <= 0) {
            break;
        }
        filled += n;
    }
    throw std::runtime_error("could not fill socket send queue");
}

} // namespace

class IdleSessionTest : public ControlledReactorTest {
protected:
    void SetUp() override {
        createReactor({.idle = 60s}, clock.source(), observation.operations());
    }

    StreamObservation observation{clock};
};

class SystemIdleSessionTest : public ProxyReactorTest {
    void SetUp() override { createReactor({.idle = 60s}); }
};

TEST_F(ControlledReactorTest, IdleDisabledPerformsNoClockReadsDuringForwardingOrTeardown) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    ASSERT_EQ(1u, clock.reads()); // Only the connection handoff checks time.
    EXPECT_FALSE(nextDeadline());
    clock.advanceTo(clock.now() + 1h);
    ASSERT_TRUE(expire(clock.now()));

    ASSERT_EQ(1, send(client.peer.get(), "d", 1, 0));
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLRDHUP}));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('d', byte);
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));

    // Queue residual data on hangup and flush it through writable dispatch.
    ASSERT_EQ(1, send(backend.peer.get(), "r", 1, 0));
    ASSERT_EQ(0, shutdown(backend.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLHUP}));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLOUT}));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('r', byte);
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    EXPECT_EQ(1u, clock.reads());
}

TEST_F(IdleSessionTest, WouldBlockReadinessAndHalfCloseLeaveInitialIdleDeadlineIntact) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto before = clock.now();
    auto ids = install(client, backend);
    auto at = nextDeadline();
    ASSERT_TRUE(at);
    EXPECT_EQ(*at, before + 60s);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLOUT}));
        EXPECT_EQ(at, nextDeadline());
    }
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLRDHUP}));
    char byte;
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ(at, nextDeadline());
    clock.advanceTo(*at - 1ns);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(registered(ids.downstream));
    clock.advanceTo(*at);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
}

TEST_F(IdleSessionTest, TrafficInEitherDirectionRefreshesOnlyItsOwnSession) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto busy = install(client, backend);
    auto quiet_client = test::socketPair();
    auto quiet_backend = test::socketPair();
    auto quiet = install(quiet_client, quiet_backend);
    auto previous = clock.now() + 5s;
    setIdleAt(busy.downstream, previous);
    setIdleAt(quiet.upstream, previous);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch({busy.downstream, EPOLLIN}));
    clock.advanceTo(previous);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_FALSE(registered(quiet.downstream));
    ASSERT_TRUE(registered(busy.upstream));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);

    const auto reverse_deadline = clock.now() + 5s;
    setIdleAt(busy.upstream, reverse_deadline);
    ASSERT_EQ(1, send(backend.peer.get(), "y", 1, 0));
    ASSERT_TRUE(dispatch({busy.upstream, EPOLLIN}));
    clock.advanceTo(reverse_deadline);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(registered(busy.downstream));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('y', byte);
    ASSERT_TRUE(nextDeadline());
    EXPECT_GT(*nextDeadline(), reverse_deadline);
}

TEST_F(SystemIdleSessionTest, QueuedWriteRefreshesOnlyOnSuccessfulSendAndPausedSessionCanExpire) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int limit = 4096;
    ASSERT_EQ(0, setsockopt(backend.local.get(), SOL_SOCKET, SO_SNDBUF, &limit, sizeof(limit)));
    auto ids = install(client, backend);
    std::array<char, 4096> payload{};
    for (int i = 0; i < 100 && interests(ids.downstream).downstream.readable; ++i) {
        ASSERT_EQ(ssize_t(payload.size()),
                  send(client.peer.get(), payload.data(), payload.size(), 0));
        ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    }
    ASSERT_FALSE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);

    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.downstream, at);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    ASSERT_EQ(at, nextDeadline()); // A copied writable flag yielded EAGAIN.
    ASSERT_GT(recv(backend.peer.get(), payload.data(), payload.size(), 0), 0);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    ASSERT_TRUE(nextDeadline());
    ASSERT_GT(*nextDeadline(), at); // Only the queued send made progress.
    ASSERT_FALSE(interests(ids.downstream).downstream.readable);

    // Leave real HUP and unread input on the paused source. The timer must wake
    // the reactor without an unbounded stream of unconditional HUP notifications.
    ASSERT_EQ(1, send(client.peer.get(), "h", 1, 0));
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_RDWR));
    setIdleAt(ids.downstream, DeadlineClock::now() + 50ms);
    ASSERT_TRUE(arm());
    int hangups = 0;
    for (int turn = 0; turn < 5 && registered(ids.downstream); ++turn) {
        auto ready = waitForEvents();
        ASSERT_TRUE(ready);
        for (auto event : *ready) {
            if (event.source_id == ids.downstream && (event.events & EPOLLHUP)) {
                ++hangups;
            }
            ASSERT_TRUE(dispatch(event));
        }
    }
    EXPECT_GE(hangups, 1);
    EXPECT_LE(hangups, 2); // Initial HUP plus a possible edge on EPOLL_CTL_MOD.
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
}

TEST_F(IdleSessionTest, ResidualReadAndQueuedSendEachCountAsSocketActivity) {
    auto source = test::socketPair();
    auto destination = test::socketPair();
    auto ids = install(source, destination);
    auto previous = clock.now() + 5s;
    setIdleAt(ids.upstream, previous);
    ASSERT_EQ(1, send(source.peer.get(), "h", 1, 0));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP})); // One residual receive, queued in full.
    ASSERT_TRUE(nextDeadline());
    ASSERT_GT(*nextDeadline(), previous);
    setIdleAt(ids.upstream, previous);
    ASSERT_TRUE(
        dispatch({ids.upstream, EPOLLOUT})); // Queued send followed by a would-block receive.
    ASSERT_TRUE(nextDeadline());
    EXPECT_GT(*nextDeadline(), previous);
    char byte;
    ASSERT_EQ(1, recv(destination.peer.get(), &byte, 1, 0));
    EXPECT_EQ('h', byte);
}

TEST_F(IdleSessionTest, ExpiredSessionCannotBeRefreshedByCopiedReadiness) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    const auto at = clock.now() + 1ms;
    setIdleAt(ids.downstream, at);
    clock.advanceTo(at);
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
}

TEST_F(IdleSessionTest, IdleExpiryBetweenCombinedReadAndWriteSkipsQueuedSend) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int backend_fd = backend.local.get();
    size_t filler = fillSendQueue(backend_fd);
    ASSERT_GT(filler, 0u);
    auto ids = install(client, backend);
    ASSERT_EQ(1, send(client.peer.get(), "q", 1, 0));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);
    ASSERT_TRUE(interests(ids.upstream).upstream.readable);
    std::array<char, 1024> buffer{};
    while (filler > 0) {
        auto n = recv(backend.peer.get(), buffer.data(), buffer.size(), 0);
        ASSERT_GT(n, 0);
        ASSERT_LE(static_cast<size_t>(n), filler);
        filler -= n;
    }

    auto at = clock.now() + 5s;
    setIdleAt(ids.upstream, at);
    clock.advanceTo(at - 1ns);
    observation.received = {.fd = backend_fd, .advance_to = at + 1ns};
    observation.sent = {.fd = backend_fd};
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN | EPOLLOUT}));
    EXPECT_EQ(1, observation.received.calls);
    EXPECT_EQ(-1, observation.received.result);
    EXPECT_EQ(EAGAIN, observation.received.error);
    EXPECT_EQ(at + 1ns, clock.now());
    EXPECT_EQ(0, observation.sent.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(watching(ids.downstream));
    EXPECT_FALSE(watching(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    EXPECT_EQ(0, recv(backend.peer.get(), buffer.data(), 1, 0));
}

TEST_F(IdleSessionTest, ReceiveObservedAtExpiryCannotRefreshDeadlineOrStartDirectSend) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int client_fd = client.local.get();
    int backend_fd = backend.local.get();
    auto ids = install(client, backend);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    auto at = clock.now() + 5s;
    setIdleAt(ids.downstream, at);
    clock.advanceTo(at - 1ns);
    observation.received = {.fd = client_fd, .advance_to = at};
    observation.sent = {.fd = backend_fd};

    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_EQ(1, observation.received.calls);
    EXPECT_EQ(1, observation.received.result);
    EXPECT_EQ(0, observation.sent.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
}

TEST_F(IdleSessionTest, ReceiveObservedBeforeExpiryRefreshesDeadline) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int client_fd = client.local.get();
    int backend_fd = backend.local.get();
    auto ids = install(client, backend);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    auto at = clock.now() + 5s;
    setIdleAt(ids.downstream, at);
    clock.advanceTo(at - 2ns);
    observation.received = {.fd = client_fd, .advance_to = at - 1ns};
    observation.sent = {.fd = backend_fd};

    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_EQ(1, observation.received.calls);
    EXPECT_EQ(1, observation.received.result);
    EXPECT_EQ(1, observation.sent.calls);
    EXPECT_EQ(1, observation.sent.result);
    EXPECT_EQ(at - 1ns + 60s, nextDeadline());
    clock.advanceTo(at);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(registered(ids.downstream));
    EXPECT_TRUE(registered(ids.upstream));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);
}

TEST_F(IdleSessionTest, QueuedSendObservedAtExpiryCompletesWithoutRevivingSession) {
    auto source = test::socketPair();
    auto destination = test::socketPair();
    int source_fd = source.local.get();
    int destination_fd = destination.local.get();
    auto ids = install(source, destination);
    ASSERT_EQ(1, send(source.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);
    auto at = clock.now() + 5s;
    setIdleAt(ids.upstream, at);
    clock.advanceTo(at - 1ns);
    observation.received = {.fd = source_fd};
    observation.sent = {.fd = destination_fd, .advance_to = at};

    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_EQ(1, observation.sent.calls);
    EXPECT_EQ(1, observation.sent.result);
    EXPECT_EQ(0, observation.received.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
    char byte;
    ASSERT_EQ(1, recv(destination.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);
    EXPECT_EQ(0, recv(destination.peer.get(), &byte, 1, 0));
}

TEST_F(IdleSessionTest, ExpiryAfterWouldBlockSendStopsDeferredHangupRead) {
    auto source = test::socketPair();
    auto destination = test::socketPair();
    int source_fd = source.local.get();
    int destination_fd = destination.local.get();
    ASSERT_GT(fillSendQueue(destination_fd), 0u);
    auto ids = install(source, destination);
    std::array<char, 4097> payload{};
    ASSERT_EQ(ssize_t(payload.size()), send(source.peer.get(), payload.data(), payload.size(), 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    ASSERT_TRUE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);
    char residual;
    ASSERT_EQ(1, recv(source_fd, &residual, 1, MSG_PEEK));
    auto at = clock.now() + 5s;
    setIdleAt(ids.upstream, at);
    clock.advanceTo(at - 1ns);
    observation.received = {.fd = source_fd};
    observation.sent = {.fd = destination_fd, .advance_to = at};

    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_EQ(1, observation.sent.calls);
    EXPECT_EQ(-1, observation.sent.result);
    EXPECT_EQ(EAGAIN, observation.sent.error);
    EXPECT_EQ(0, observation.received.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
}

TEST_F(IdleSessionTest, SuccessfulQueuedSendRefreshesBeforeDeferredHangupRead) {
    auto source = test::socketPair();
    auto destination = test::socketPair();
    const int source_fd = source.local.get();
    const int destination_fd = destination.local.get();
    auto ids = install(source, destination);
    std::array<char, 4097> payload{};
    payload.fill('p');
    payload.back() = 'z';
    ASSERT_EQ(ssize_t(payload.size()), send(source.peer.get(), payload.data(), payload.size(), 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    ASSERT_TRUE(interests(ids.downstream).downstream.readable);
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);

    const auto at = clock.now() + 5s;
    setIdleAt(ids.upstream, at);
    clock.advanceTo(at - 1ns);
    observation.received = {.fd = source_fd};
    // Let the send's activity check see valid time, then cross the original deadline.
    observation.sent = {.fd = destination_fd, .advance_after_read = at};
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_EQ(1, observation.sent.calls);
    EXPECT_EQ(4096, observation.sent.result);
    EXPECT_EQ(at - 1ns, observation.sent.observed_at);
    EXPECT_EQ(1, observation.received.calls);
    EXPECT_EQ(1, observation.received.result);
    EXPECT_EQ(at, observation.received.observed_at);
    EXPECT_EQ(at + 60s, nextDeadline());
    ASSERT_TRUE(registered(ids.upstream));
    ASSERT_TRUE(interests(ids.upstream).upstream.writable);

    std::array<char, 4096> prefix{};
    ASSERT_EQ(ssize_t(prefix.size()),
              recv(destination.peer.get(), prefix.data(), prefix.size(), 0));
    EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), payload.begin()));
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    char byte;
    ASSERT_EQ(1, recv(destination.peer.get(), &byte, 1, 0));
    EXPECT_EQ(payload.back(), byte);
    EXPECT_EQ(0, recv(destination.peer.get(), &byte, 1, 0));
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
}

TEST_F(IdleSessionTest, RemovedSessionDeadlineCannotCloseReplacement) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int old_fd = client.local.get();
    auto old = install(client, backend);
    auto at = clock.now() + 5s;
    setIdleAt(old.downstream, at);
    ASSERT_TRUE(dispatch({old.downstream, EPOLLERR}));
    ASSERT_FALSE(nextDeadline());
    auto replacement = test::socketPair();
    auto new_backend = test::socketPair();
    ASSERT_EQ(old_fd, replacement.local.get());
    auto live = install(replacement, new_backend);
    clock.advanceTo(at);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(registered(live.downstream));
    ASSERT_TRUE(shutdownRequest());
    ASSERT_TRUE(shutdownRequest());
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(live.upstream));
}

TEST_F(IdleSessionTest, IdleExpiryFinishesGracefulShutdown) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    auto at = clock.now() + 5s;
    setIdleAt(ids.upstream, at);
    ASSERT_TRUE(shutdownRequest());
    EXPECT_FALSE(stopped());
    clock.advanceTo(at);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(ids.upstream));
}

TEST_F(IdleSessionTest, DueGracefulShutdownTakesPrecedenceOverAnIdleBurst) {
    auto idle_at = clock.now() + 5s;
    for (int i = 0; i < 65; ++i) {
        auto client = test::socketPair();
        auto backend = test::socketPair();
        auto ids = install(client, backend);
        setIdleAt(ids.downstream, idle_at);
    }
    auto longer_client = test::socketPair();
    auto longer_backend = test::socketPair();
    auto longer = install(longer_client, longer_backend);
    ASSERT_TRUE(shutdownRequest());
    clock.advanceTo(clock.now() + 30s);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(registered(longer.downstream));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(longer_client.peer.get(), &byte, 1, 0));
}

} // namespace orbit::proxy
