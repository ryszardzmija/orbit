#include <array>
#include <cerrno>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <sys/socket.h>

#include "proxy/detail/session/session_manager.h"

namespace orbit::proxy::detail {
namespace {

struct SocketPair {
    FileDescriptor local;
    FileDescriptor peer;
};

SocketPair socketPair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
        throw std::system_error(errno, std::system_category(), "socketpair");
    }
    return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

class HangupTest : public testing::TestWithParam<EndpointRole> {};

TEST_P(HangupTest, PausedQueuePreservesResidualInputUntilTheDestinationDrains) {
    SessionManager manager(
        {.send_buffer = {4, 9, 4}, .forwarder_buffer_capacity = 4, .sender_buffer_capacity = 4});
    auto downstream = socketPair();
    auto upstream = socketPair();
    const EndpointRole role = GetParam();
    const bool from_client = role == EndpointRole::Downstream;
    const EndpointRole other = from_client ? EndpointRole::Upstream : EndpointRole::Downstream;
    auto& source = from_client ? downstream : upstream;
    auto& dest = from_client ? upstream : downstream;
    int source_fd = source.local.get();
    int limit = 4096;
    ASSERT_EQ(0, setsockopt(dest.local.get(), SOL_SOCKET, SO_SNDBUF, &limit, sizeof(limit)));
    std::array<char, 1024> scratch{};
    size_t filler = 0;
    while (filler < 1024 * 1024) {
        ssize_t n = send(dest.local.get(), scratch.data(), scratch.size(), MSG_NOSIGNAL);
        if (n < 0) {
            ASSERT_EQ(EAGAIN, errno);
            break;
        }
        ASSERT_GT(n, 0);
        filler += n;
    }
    ASSERT_GT(filler, 0u);
    ASSERT_LT(filler, 1024 * 1024u);

    auto id = manager.allocateId();
    manager.add(id, std::move(downstream.local), std::move(upstream.local), {1, 2});
    auto source_interests = [&] {
        return from_client ? manager.interests(id).downstream : manager.interests(id).upstream;
    };
    std::string payload = "0123456789abcdefghijklmnopqrstuvwxyz!";
    ASSERT_EQ(ssize_t(payload.size()), send(source.peer.get(), payload.data(), payload.size(), 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_RDWR));
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(source_interests().readable);
        ASSERT_EQ(SessionEventAction::KeepOpen, manager.handleReadable(id, role).action);
    }
    ASSERT_FALSE(source_interests().readable); // 12 queued, high watermark 9.
    ASSERT_EQ(SessionEventAction::KeepOpen, manager.handleHangup(id, role).action);
    EXPECT_TRUE(source_interests().edge_triggered);
    EXPECT_FALSE(manager.shouldRetire(id, role));
    auto residual = [&] { return recv(source_fd, scratch.data(), scratch.size(), MSG_PEEK); };
    ASSERT_EQ(ssize_t(payload.size() - 12), residual());
    for (size_t left = filler; left > 0;) {
        ssize_t n = recv(dest.peer.get(), scratch.data(), std::min(left, scratch.size()), 0);
        ASSERT_GT(n, 0);
        left -= n;
    }
    ASSERT_EQ(SessionEventAction::KeepOpen, manager.handleWritable(id, other).action);
    ASSERT_EQ(ssize_t(payload.size() - 12), residual()); // Still above low watermark.
    ASSERT_EQ(SessionEventAction::KeepOpen, manager.handleWritable(id, other).action);
    ASSERT_EQ(ssize_t(payload.size() - 16), residual()); // One residual chunk this dispatch.

    auto action = SessionEventAction::KeepOpen;
    for (size_t i = 0; i < payload.size() && action == SessionEventAction::KeepOpen; ++i) {
        auto result = manager.handleWritable(id, other);
        ASSERT_FALSE(result.error) << result.error.value_or("");
        action = result.action;
    }
    ASSERT_EQ(SessionEventAction::Close, action);
    manager.remove(id);
    ASSERT_EQ(ssize_t(payload.size()), recv(dest.peer.get(), scratch.data(), scratch.size(), 0));
    EXPECT_EQ(payload, std::string(scratch.data(), payload.size()));
    EXPECT_EQ(0, recv(dest.peer.get(), scratch.data(), scratch.size(), 0));
}

INSTANTIATE_TEST_SUITE_P(BothDirections, HangupTest,
                         testing::Values(EndpointRole::Downstream, EndpointRole::Upstream));

TEST(SessionManagerTest, WouldBlockAfterHangupKeepsInputEligibleForANewEdge) {
    SessionManager manager({{4, 8, 4}, 4, 4});
    auto source = socketPair();
    auto dest = socketPair();
    auto id = manager.allocateId();
    manager.add(id, std::move(source.local), std::move(dest.local), {1, 2});
    EXPECT_EQ(SessionEventAction::KeepOpen,
              manager.handleHangup(id, EndpointRole::Downstream).action);
    EXPECT_TRUE(manager.interests(id).downstream.readable);
    EXPECT_TRUE(manager.interests(id).downstream.edge_triggered);
    EXPECT_FALSE(manager.interests(id).upstream.writable);
    ASSERT_EQ(1, send(source.peer.get(), "x", 1, 0));
    ASSERT_EQ(0, shutdown(source.peer.get(), SHUT_WR));
    EXPECT_EQ(SessionEventAction::KeepOpen,
              manager.handleReadable(id, EndpointRole::Downstream).action);
    EXPECT_TRUE(manager.interests(id).upstream.writable);
    EXPECT_EQ(SessionEventAction::Close, manager.handleWritable(id, EndpointRole::Upstream).action);
    char byte;
    ASSERT_EQ(1, recv(dest.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);
    EXPECT_EQ(0, recv(dest.peer.get(), &byte, 1, 0));
}

} // namespace
} // namespace orbit::proxy::detail
