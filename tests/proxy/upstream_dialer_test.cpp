#include <chrono>
#include <stdexcept>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "controlled_clock.h"
#include "net/listener.h"
#include "proxy/detail/upstream_dialer.h"
#include "socket_pair.h"

namespace {

struct SocketPreparation;
thread_local SocketPreparation* preparing_socket = nullptr;

struct SocketPreparation {
    SocketPreparation(orbit::proxy::test::ControlledClock& clock,
                      orbit::proxy::detail::Deadline expires_at)
        : clock(clock),
          expires_at(expires_at) {
        preparing_socket = this;
    }

    ~SocketPreparation() { preparing_socket = nullptr; }

    orbit::proxy::test::ControlledClock& clock;
    orbit::proxy::detail::Deadline expires_at;
    int prepared_fd = -1;
};

} // namespace

extern "C" int __real_socket(int domain, int type, int protocol) noexcept;
extern "C" int __wrap_socket(int domain, int type, int protocol) noexcept {
    int fd = __real_socket(domain, type, protocol);
    if (preparing_socket && fd >= 0) {
        preparing_socket->prepared_fd = fd;
        preparing_socket->clock.advanceTo(preparing_socket->expires_at);
        preparing_socket = nullptr;
    }
    return fd;
}

namespace orbit::proxy::detail {

using namespace std::chrono_literals;

class UpstreamDialerTest : public testing::Test {
protected:
    // Supply two resolved addresses independently of the host's DNS configuration.
    static UpstreamDialer withCandidates(const net::SocketAddress& candidate) {
        return UpstreamDialer(ResolvedUpstream({"backend", 1}, {candidate, candidate}));
    }

    static PendingConnection failedAttempt(FileDescriptor client, Deadline deadline) {
        // SO_ERROR fails deterministically for this descriptor, driving address fallback.
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
        return {std::move(client), {}, FileDescriptor(fd), 0, deadline, {}};
    }
};

TEST_F(UpstreamDialerTest, FallbackPreservesOverallDeadlineThroughConnectCompletion) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    auto dialer = withCandidates(listener->localAddress());
    auto at = DeadlineClock::now() + 10s;
    auto result = dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at));
    ASSERT_TRUE(result) << result.error().error_messages;
    if (auto* attempt = std::get_if<UpstreamDialInProgress>(&*result)) {
        EXPECT_EQ(1u, attempt->pending_connection.attempted_address_idx);
        EXPECT_EQ(at, attempt->pending_connection.deadline);
        pollfd ready{attempt->pending_connection.attempted_connection_fd->get(), POLLOUT, 0};
        ASSERT_EQ(1, poll(&ready, 1, 1000));
        result = dialer.advanceDialAttempt(std::move(attempt->pending_connection));
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
    auto dialer = withCandidates(listener->localAddress());
    auto at = DeadlineClock::now() - 1ms;
    auto client = test::socketPair();
    EXPECT_FALSE(dialer.dial(std::move(client.local), {}, at));
    EXPECT_FALSE(dialer.advanceDialAttempt(failedAttempt(test::socketPair().local, at)));
    auto accepted = listener->acceptClientConnection();
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<net::AcceptWouldBlock>(*accepted));
}

TEST_F(UpstreamDialerTest, DeadlineExpiringDuringFallbackSocketPreparationSkipsConnect) {
    auto listener = net::Listener::create({"127.0.0.1", 0}, 4);
    ASSERT_TRUE(listener);
    auto dialer = withCandidates(listener->localAddress());
    const auto at = DeadlineClock::now() + 10s;
    auto client = test::socketPair();
    auto failed = failedAttempt(std::move(client.local), at);
    test::ControlledClock clock(at - 1ms);
    SocketPreparation preparation{clock, at};

    auto result = dialer.advanceDialAttempt(std::move(failed));

    ASSERT_GE(preparation.prepared_fd, 0);
    EXPECT_EQ(at, DeadlineClock::now());
    auto accepted = listener->acceptClientConnection();
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<net::AcceptWouldBlock>(*accepted));
    ASSERT_FALSE(result);
    EXPECT_FALSE(result.error().socket_creation_error);
    EXPECT_EQ("overall connection establishment deadline expired", result.error().error_messages);
    int flags = fcntl(preparation.prepared_fd, F_GETFD);
    int fd_error = errno;
    EXPECT_EQ(-1, flags);
    EXPECT_EQ(EBADF, fd_error);
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
}

} // namespace orbit::proxy::detail
