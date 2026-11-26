#include <cerrno>
#include <chrono>
#include <optional>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
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
                                         net::DialOperations operations) {
        return UpstreamDialer(ResolvedUpstream({"backend", 1}, {candidate, candidate}), operations);
    }

    static PendingConnection failedAttempt(FileDescriptor client, Deadline deadline) {
        // SO_ERROR fails deterministically for this descriptor, driving address fallback.
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
        return {std::move(client), {}, FileDescriptor(fd), 0, deadline, {}};
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
    auto result =
        dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at), time_source);
    ASSERT_TRUE(result) << result.error().error_messages;
    EXPECT_EQ(2, observation.socket_calls);
    EXPECT_EQ(2, observation.connect_calls);
    EXPECT_EQ(at - 1ms, clock.now());
    if (auto* attempt = std::get_if<UpstreamDialInProgress>(&*result)) {
        EXPECT_EQ(1u, attempt->pending_connection.attempted_address_idx);
        EXPECT_EQ(at, attempt->pending_connection.deadline);
        pollfd ready{attempt->pending_connection.attempted_connection_fd->get(), POLLOUT, 0};
        ASSERT_EQ(1, poll(&ready, 1, 1000));
        result = dialer.advanceDialAttempt(std::move(attempt->pending_connection), time_source);
        ASSERT_TRUE(result) << result.error().error_messages;
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
    EXPECT_FALSE(dialer.dial(std::move(client.local), {}, at, time_source));
    EXPECT_FALSE(
        dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at), time_source));
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

    auto result = dialer.dial(std::move(client.local), {}, at, clock.source());

    ASSERT_FALSE(result);
    EXPECT_FALSE(result.error().socket_creation_error);
    EXPECT_EQ("overall connection establishment deadline expired", result.error().error_messages);
    EXPECT_EQ(at, clock.now());
    EXPECT_EQ(1, observation.socket_calls);
    EXPECT_EQ(1, observation.connect_calls);
    expectClosedDescriptor(observation.prepared_fd);
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
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

} // namespace orbit::proxy
