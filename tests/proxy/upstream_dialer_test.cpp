#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "controlled_clock.h"
#include "net/listener.h"
#include "proxy/detail/upstream_dialer.h"
#include "reactor_fixture.h"
#include "socket_pair.h"

namespace orbit::proxy {

using namespace std::chrono_literals;

namespace {

struct DialObservation {
    test::ControlledClock& clock;
    std::optional<detail::Deadline> advance_after_socket;
    std::optional<detail::Deadline> interrupt_connect_at;
    int socket_calls = 0;
    int connect_calls = 0;
    int prepared_fd = -1;

    net::DialOperations operations() {
        return {
            .context = this,
            .socket =
                [](void* context, int domain, int type, int protocol) {
                    auto& dial = *static_cast<DialObservation*>(context);
                    ++dial.socket_calls;
                    int fd = ::socket(domain, type, protocol);
                    if (fd >= 0) {
                        dial.prepared_fd = fd;
                        if (dial.advance_after_socket) {
                            dial.clock.advanceTo(*dial.advance_after_socket);
                        }
                    }
                    return fd;
                },
            .connect =
                [](void* context, int fd, const sockaddr* address, socklen_t length) {
                    auto& dial = *static_cast<DialObservation*>(context);
                    ++dial.connect_calls;
                    if (auto at = std::exchange(dial.interrupt_connect_at, std::nullopt)) {
                        dial.clock.advanceTo(*at);
                        errno = EINTR;
                        return -1;
                    }
                    return ::connect(fd, address, length);
                },
        };
    }
};

void expectClosedDescriptor(int fd) {
    ASSERT_GE(fd, 0);
    const int flags = fcntl(fd, F_GETFD);
    const int error = errno;
    EXPECT_EQ(-1, flags);
    EXPECT_EQ(EBADF, error);
}

} // namespace

namespace detail {

class UpstreamDialerTest : public testing::Test {
public:
    // Supply two resolved candidates independently of the host's DNS configuration.
    static UpstreamDialer withCandidates(const net::SocketAddress& candidate,
                                         net::DialOperations operations, size_t count = 2) {
        return UpstreamDialer(
            ResolvedUpstream({"backend", 1}, std::vector<net::SocketAddress>(count, candidate)),
            operations);
    }

    static PendingConnection failedAttempt(FileDescriptor client, Deadline deadline) {
        // SO_ERROR fails deterministically for this descriptor, driving address fallback.
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
        return {std::move(client), {}, FileDescriptor(fd), 0, deadline};
    }

protected:
    test::ControlledClock clock;
    DialObservation observation{clock};
};

TEST_F(UpstreamDialerTest, FallbackPreservesOverallDeadlineThroughConnectCompletion) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    auto dialer = withCandidates(listener->localAddress(), observation.operations());
    const auto time_source = clock.source();
    const auto at = clock.now() + 10s;
    // Interrupt the first connect on the fallback candidate, then retry that same address.
    observation.interrupt_connect_at = at - 1ms;
    size_t attempts = 8;
    auto result = dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at),
                                            time_source, attempts);
    ASSERT_TRUE(result) << result.error().code.message();
    EXPECT_EQ(2, observation.socket_calls);
    EXPECT_EQ(2, observation.connect_calls);
    EXPECT_EQ(at - 1ms, clock.now());
    if (auto* attempt = std::get_if<UpstreamDialInProgress>(&*result)) {
        EXPECT_EQ(1u, attempt->pending_connection.attempted_address_idx);
        EXPECT_EQ(at, attempt->pending_connection.deadline);
        pollfd ready{attempt->pending_connection.attempted_connection_fd->get(), POLLOUT, 0};
        ASSERT_EQ(1, poll(&ready, 1, 1000));
        result = dialer.advanceDialAttempt(std::move(attempt->pending_connection), time_source,
                                           attempts);
        ASSERT_TRUE(result) << result.error().code.message();
    }
    ASSERT_TRUE(std::holds_alternative<UpstreamDialConnected>(*result));
    EXPECT_EQ(at, std::get<UpstreamDialConnected>(*result).deadline);
    auto accepted = listener->acceptClientConnection();
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<net::AcceptSuccess>(*accepted));
}

TEST_F(UpstreamDialerTest, ExpiredBudgetStartsNeitherFirstNorFallbackAddress) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    auto dialer = withCandidates(listener->localAddress(), observation.operations());
    const auto time_source = clock.source();
    const auto at = clock.now();
    clock.advanceTo(at + 1ms);
    auto client = test::socketPair();
    size_t attempts = 8;
    EXPECT_FALSE(dialer.dial(std::move(client.local), {}, at, time_source, attempts));
    EXPECT_FALSE(dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at), time_source,
                                           attempts));
    EXPECT_EQ(0, observation.socket_calls);
    EXPECT_EQ(0, observation.connect_calls);
    auto accepted = listener->acceptClientConnection();
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<net::AcceptWouldBlock>(*accepted));
}

TEST_F(UpstreamDialerTest, InterruptedConnectAtDeadlineDoesNotPrepareAnotherSocket) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    auto dialer = withCandidates(listener->localAddress(), observation.operations());
    auto client = test::socketPair();
    const auto at = clock.now() + 1ms;
    observation.interrupt_connect_at = at;

    size_t attempts = 8;
    auto result = dialer.dial(std::move(client.local), {}, at, clock.source(), attempts);

    ASSERT_FALSE(result);
    EXPECT_EQ(net::DialFailedOp::Deadline, result.error().failed_op);
    EXPECT_EQ(std::errc::timed_out, result.error().code);
    EXPECT_EQ(at, clock.now());
    EXPECT_EQ(1, observation.socket_calls);
    EXPECT_EQ(1, observation.connect_calls);
    expectClosedDescriptor(observation.prepared_fd);
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
}

TEST_F(UpstreamDialerTest, ResumingALargeCandidateSetPreservesItsPositionAndOriginalDeadline) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    int code = ECONNREFUSED;
    auto dialer = withCandidates(listener->localAddress(),
                                 {
                                     .context = &code,
                                     .connect =
                                         [](void* context, int, const sockaddr*, socklen_t) {
                                             errno = *static_cast<int*>(context);
                                             return errno == 0 ? 0 : -1;
                                         },
                                 },
                                 512);
    auto client = test::socketPair();
    const auto at = clock.now() + 5s;
    size_t attempts = 8;
    auto result = dialer.dial(std::move(client.local), {}, at, clock.source(), attempts);
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<UpstreamDialDeferred>(*result));
    EXPECT_FALSE(std::get<UpstreamDialDeferred>(*result).interrupted);
    auto pending = std::move(std::get<UpstreamDialDeferred>(*result).pending_connection);
    EXPECT_EQ(8u, pending.attempted_address_idx);
    EXPECT_EQ(at, pending.deadline);
    EXPECT_FALSE(pending.attempted_connection_fd);
    EXPECT_EQ(0u, attempts);

    code = EINTR;
    attempts = 4;
    result = dialer.resumeDial(std::move(pending), clock.source(), attempts);
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<UpstreamDialDeferred>(*result));
    EXPECT_TRUE(std::get<UpstreamDialDeferred>(*result).interrupted);
    pending = std::move(std::get<UpstreamDialDeferred>(*result).pending_connection);
    EXPECT_EQ(8u, pending.attempted_address_idx);
    EXPECT_EQ(0u, attempts);
    EXPECT_EQ(at, pending.deadline);

    code = 0;
    attempts = 1;
    result = dialer.resumeDial(std::move(pending), clock.source(), attempts);
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<UpstreamDialConnected>(*result));
    EXPECT_EQ(at, std::get<UpstreamDialConnected>(*result).deadline);
}

} // namespace detail

class FallbackReactorTest : public ControlledReactorTest {
protected:
    DialObservation observation{clock};
};

TEST_F(FallbackReactorTest, DeadlineExpiringDuringFallbackClosesAttemptAndKeepsEstablishedTraffic) {
    createReactor({}, clock.source(), {}, {.max_connections = 2, .max_pending_connections = 1});
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(listener->localAddress(),
                                                                 observation.operations()));

    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto live = install(client, backend);
    char byte;
    ASSERT_EQ(1, send(client.peer.get(), "a", 1, 0));
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('a', byte);

    auto waiting = test::socketPair();
    const auto at = clock.now() + 1ms;
    auto attempt = detail::UpstreamDialerTest::failedAttempt(std::move(waiting.local), at);
    const int failed_fd = attempt.attempted_connection_fd->get();
    auto pending = installPending(std::move(attempt));
    ASSERT_EQ(at, nextDeadline());
    ASSERT_TRUE(watching(pending));
    ASSERT_FALSE(listenerAccepting());
    observation.advance_after_socket = at;
    clock.advanceTo(at - 1ns);

    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_EQ(at, clock.now());
    EXPECT_EQ(1, observation.socket_calls);
    EXPECT_EQ(0, observation.connect_calls);
    expectClosedDescriptor(observation.prepared_fd);
    expectClosedDescriptor(failed_fd);
    EXPECT_EQ(0, recv(waiting.peer.get(), &byte, 1, 0));
    EXPECT_FALSE(registered(pending));
    EXPECT_FALSE(watching(pending));
    EXPECT_FALSE(nextDeadline());
    EXPECT_TRUE(listenerAccepting());
    auto accepted = listener->acceptClientConnection();
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<net::AcceptWouldBlock>(*accepted));

    ASSERT_TRUE(registered(live.downstream));
    ASSERT_TRUE(registered(live.upstream));
    ASSERT_EQ(1, send(backend.peer.get(), "b", 1, 0));
    ASSERT_TRUE(dispatch({live.upstream, EPOLLIN}));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('b', byte);
}

TEST_F(FallbackReactorTest, ImmediateAttemptsReserveAdmissionAndReleaseOnFailureOrPromotion) {
    createReactor({}, clock.source(), {}, {.max_connections = 1, .max_pending_connections = 1});
    using Fixture = decltype(this);
    struct Attempts {
        Fixture fixture;
        bool succeed = false;
        int calls = 0;
    } attempts{this};
    auto backend = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(backend);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        backend->localAddress(), {
                                     .context = &attempts,
                                     .connect =
                                         [](void* context, int, const sockaddr*, socklen_t) {
                                             auto& state = *static_cast<Attempts*>(context);
                                             EXPECT_EQ(1u, state.fixture->pendingCount());
                                             EXPECT_EQ(0u, state.fixture->sessionCount());
                                             ++state.calls;
                                             if (state.succeed) {
                                                 return 0;
                                             }
                                             errno = ECONNREFUSED;
                                             return -1;
                                         },
                                 }));

    auto failed = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(2, attempts.calls);
    EXPECT_EQ(0u, pendingCount());
    EXPECT_TRUE(listenerAccepting());
    char byte;
    ASSERT_EQ(0, recv(failed.get(), &byte, 1, 0));

    attempts.succeed = true;
    auto connected = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(3, attempts.calls);
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(1u, sessionCount());
    EXPECT_FALSE(listenerAccepting());

    auto queued = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(3, attempts.calls);
    ASSERT_TRUE(dispatch({currentSessionSources().upstream, EPOLLERR}));
    EXPECT_EQ(0u, sessionCount());
    EXPECT_TRUE(listenerAccepting());
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(4, attempts.calls);
    EXPECT_EQ(1u, sessionCount());
}

TEST_F(FallbackReactorTest, DialDescriptorPressureDropsThatClientAndBlocksFurtherAttempts) {
    createReactor({}, clock.source(), {}, {.max_connections = 2, .max_pending_connections = 1});
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    auto backend = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(backend);
    struct Failure {
        int calls = 0;
        int code = EMFILE;
    } fault;
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        backend->localAddress(),
        {
            .context = &fault,
            .socket =
                [](void* context, int domain, int type, int protocol) {
                    auto& state = *static_cast<Failure*>(context);
                    ++state.calls;
                    if (state.code != 0) {
                        errno = state.code;
                        return -1;
                    }
                    return socket(domain, type, protocol);
                },
            .connect = [](void*, int, const sockaddr*, socklen_t) { return 0; },
        }));
    int attempts = 0;
    char byte;
    for (auto error : {EMFILE, ENFILE}) {
        fault.code = error;
        auto incoming = connectToListener();
        ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
        EXPECT_EQ(++attempts, fault.calls);
        EXPECT_EQ(0u, pendingCount());
        EXPECT_EQ(0, recv(incoming.get(), &byte, 1, 0));
        EXPECT_FALSE(listenerAccepting());
        ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
        EXPECT_EQ(attempts, fault.calls);
        ASSERT_EQ(1, send(client.peer.get(), "a", 1, 0));
        ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
        EXPECT_EQ(1, recv(upstream.peer.get(), &byte, 1, 0));
        EXPECT_EQ('a', byte);
        ASSERT_TRUE(retryAt());
        clock.advanceTo(*retryAt());
        ASSERT_TRUE(expire(clock.now()));
        EXPECT_TRUE(listenerAccepting());
    }
    fault.code = 0;
    auto incoming = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(++attempts, fault.calls);
    EXPECT_EQ(2u, sessionCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(retryAt());
}

TEST_F(FallbackReactorTest, CandidateWorkSharesABatchBudgetAndDeferredAttemptsExpireOrCancel) {
    createReactor({.connect = 20ms, .idle = 1s}, clock.source());
    int attempts = 0;
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        listener->localAddress(),
        {.context = &attempts,
         .connect =
             [](void* context, int, const sockaddr*, socklen_t) {
                 ++*static_cast<int*>(context);
                 errno = ECONNREFUSED;
                 return -1;
             }},
        512));

    auto a = test::socketPair();
    auto a_backend = test::socketPair();
    auto b = test::socketPair();
    auto b_backend = test::socketPair();
    const auto first = install(a, a_backend);
    const auto second = install(b, b_backend);
    const auto listening = listenerId();
    std::vector<FileDescriptor> backlog;
    for (int i = 0; i < 12; ++i) {
        backlog.push_back(connectToListener());
    }
    const auto at = clock.now() + 20ms;
    ASSERT_EQ(1, send(a.peer.get(), "a", 1, 0));
    ASSERT_EQ(1, send(b_backend.peer.get(), "b", 1, 0));
    const std::array batch{detail::ReadyEvent{listening, EPOLLIN},
                           detail::ReadyEvent{listening, EPOLLIN},
                           detail::ReadyEvent{first.downstream, EPOLLIN},
                           detail::ReadyEvent{second.upstream, EPOLLIN}};
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(dispatchBatch(batch));
    }
    EXPECT_EQ(80, attempts);
    EXPECT_EQ(at, nextDeadline());
    EXPECT_EQ(10u, pendingCount());
    EXPECT_EQ(10u, deferredDialCount());
    char byte;
    ASSERT_EQ(1, recv(a_backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('a', byte);
    ASSERT_EQ(1, recv(b.peer.get(), &byte, 1, 0));
    EXPECT_EQ('b', byte);

    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(88, attempts);
    EXPECT_EQ(at, nextDeadline());
    EXPECT_EQ(10u, pendingCount());
    clock.advanceTo(at);
    ASSERT_TRUE(expire(at));
    EXPECT_FALSE(hasRunnableDials());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(0u, deferredDialCount());
    EXPECT_EQ(0, recv(backlog.front().get(), &byte, 1, 0));

    ASSERT_TRUE(dispatchBatch(batch));
    EXPECT_EQ(96, attempts);
    ASSERT_EQ(1u, deferredDialCount());
    ASSERT_TRUE(shutdownRequest());
    EXPECT_EQ(0u, deferredDialCount());
    EXPECT_EQ(0u, pendingCount());
    ASSERT_TRUE(dispatchBatch(batch));
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(96, attempts);
    ASSERT_EQ(1, send(a.peer.get(), "s", 1, 0));
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    EXPECT_EQ(1, recv(a_backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('s', byte);
}

TEST_F(ProxyReactorTest, InterruptedPollStillRunsDeferredDialBeforeReturningToBlockingWait) {
    std::vector<int> timeouts;
    createReactor(
        {}, {}, {}, {},
        {.epoll = {&timeouts, nullptr, [](void* context, int, epoll_event*, int, int timeout) {
                       auto& waits = *static_cast<std::vector<int>*>(context);
                       waits.push_back(timeout);
                       if (waits.size() == 1) {
                           errno = EINTR;
                           return -1;
                       }
                       return 0;
                   }}});
    auto backend = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(backend);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        backend->localAddress(),
        {.connect = [](void*, int, const sockaddr*, socklen_t) { return 0; }}));
    auto client = test::socketPair();
    const auto at = detail::DeadlineClock::now() + 10s;
    installPending({std::move(client.local), {}, std::nullopt, 0, at});
    ASSERT_TRUE(runTurn());
    EXPECT_EQ(1u, sessionCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(nextDeadline());
    ASSERT_TRUE(runTurn());
    EXPECT_EQ((std::vector<int>{0, -1}), timeouts);
}

TEST_F(FallbackReactorTest, RepeatedDialInterruptionsParkTheClientUntilTheTimer) {
    createReactor({.connect = 10ms}, clock.source());
    struct Calls {
        int error = EINTR;
        int attempts = 0;
    } calls;
    auto backend = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(backend);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        backend->localAddress(),
        {.context = &calls, .connect = [](void* context, int, const sockaddr*, socklen_t) {
             auto& state = *static_cast<Calls*>(context);
             ++state.attempts;
             errno = state.error;
             return errno == 0 ? 0 : -1;
         }}));
    auto client = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    EXPECT_EQ(8, calls.attempts);
    EXPECT_EQ(1u, pendingCount());
    EXPECT_FALSE(hasRunnableDials());
    EXPECT_EQ(clock.now() + 1ms, nextWakeup());
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(8, calls.attempts);
    clock.advanceTo(clock.now() + 1ms);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(hasRunnableDials());
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(9, calls.attempts);
    EXPECT_EQ(1u, pendingCount());
    EXPECT_FALSE(hasRunnableDials());

    calls.error = 0;
    clock.advanceTo(clock.now() + 1ms);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(10, calls.attempts);
    EXPECT_EQ(1u, sessionCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(nextWakeup());
}

TEST_F(FallbackReactorTest, PressureParksFallbackAndRecoveryKeepsTheOriginalDeadline) {
    createReactor({}, clock.source());
    struct Pressure {
        int error = EMFILE;
        int socket_calls = 0;
    } pressure;
    auto backend = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(backend);
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        backend->localAddress(),
        {.context = &pressure,
         .socket =
             [](void* context, int domain, int type, int protocol) {
                 auto& state = *static_cast<Pressure*>(context);
                 ++state.socket_calls;
                 if (state.error != 0) {
                     errno = state.error;
                     return -1;
                 }
                 return socket(domain, type, protocol);
             },
         .connect = [](void*, int, const sockaddr*, socklen_t) { return 0; }}));

    auto waiting = test::socketPair();
    const auto at = clock.now() + 1s;
    const auto pending =
        installPending(detail::UpstreamDialerTest::failedAttempt(std::move(waiting.local), at));
    auto rejected = connectToListener();
    ASSERT_TRUE(dispatch({listenerId(), EPOLLIN}));
    ASSERT_TRUE(retryAt());
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_FALSE(registered(pending));
    EXPECT_EQ(1, pressure.socket_calls);
    EXPECT_EQ(1u, pendingCount());
    EXPECT_EQ(1u, deferredDialCount());
    EXPECT_EQ(at, nextDeadline());
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(1, pressure.socket_calls);
    EXPECT_FALSE(hasRunnableDials());

    pressure.error = 0;
    clock.advanceTo(*retryAt());
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_TRUE(hasRunnableDials());
    ASSERT_TRUE(resumeDeferredDials());
    EXPECT_EQ(2, pressure.socket_calls);
    EXPECT_EQ(1u, sessionCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(nextDeadline());
}

TEST_F(FallbackReactorTest, SignalInABusyBatchCancelsDeferredWorkAndLetsSessionsDrain) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    int attempts = 0;
    setUpstreamDialer(detail::UpstreamDialerTest::withCandidates(
        listener->localAddress(),
        {.context = &attempts,
         .connect =
             [](void* context, int, const sockaddr*, socklen_t) {
                 ++*static_cast<int*>(context);
                 errno = ECONNREFUSED;
                 return -1;
             }},
        512));
    auto client = test::socketPair();
    auto backend = test::socketPair();
    const auto live = install(client, backend);
    const auto listening = listenerId();
    auto incoming = connectToListener();

    struct SignalMask {
        sigset_t term;
        sigset_t previous;
        ~SignalMask() {
            // Drain an unconsumed test signal before restoring a possibly unblocked mask.
            const timespec no_wait{};
            sigtimedwait(&term, nullptr, &no_wait);
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
    };
    sigset_t term;
    ASSERT_EQ(0, sigemptyset(&term));
    ASSERT_EQ(0, sigaddset(&term, SIGTERM));
    sigset_t previous;
    ASSERT_EQ(0, pthread_sigmask(SIG_BLOCK, &term, &previous));
    const SignalMask mask{term, previous};
    ASSERT_EQ(0, raise(SIGTERM));

    auto ready = waitForEvents();
    ASSERT_TRUE(ready);
    std::vector<detail::ReadyEvent> batch{{listening, EPOLLIN}};
    batch.insert(batch.end(), ready->begin(), ready->end());
    ASSERT_TRUE(dispatchBatch(batch));
    EXPECT_EQ(8, attempts);
    EXPECT_FALSE(registered(listening));
    EXPECT_EQ(0u, deferredDialCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(stopped());
    ASSERT_EQ(1, send(client.peer.get(), "s", 1, 0));
    ASSERT_TRUE(dispatch({live.downstream, EPOLLIN}));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('s', byte);
}

} // namespace orbit::proxy
