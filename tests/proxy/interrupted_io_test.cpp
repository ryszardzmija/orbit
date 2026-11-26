#include <array>
#include <cerrno>
#include <chrono>
#include <string_view>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "reactor_fixture.h"

namespace orbit::proxy {
namespace {

using namespace std::chrono_literals;

class InterruptedIoTest : public ControlledReactorTest {
protected:
    void SetUp() override {
        createReactor({.idle = 10s}, clock.source(),
                      {
                          .context = this,
                          .receive =
                              [](void* context, int fd, void* bytes, size_t size, int flags) {
                                  auto& read = static_cast<InterruptedIoTest*>(context)->read;
                                  if (read.interrupt(fd)) {
                                      return ssize_t{-1};
                                  }
                                  return recv(fd, bytes, size, flags);
                              },
                          .send =
                              [](void* context, int fd, const void* bytes, size_t size, int flags) {
                                  auto& write = static_cast<InterruptedIoTest*>(context)->write;
                                  if (write.interrupt(fd)) {
                                      return ssize_t{-1};
                                  }
                                  return send(fd, bytes, size, flags);
                              },
                          .shutdown =
                              [](void* context, int fd, int how) {
                                  auto& fin = static_cast<InterruptedIoTest*>(context)->fin;
                                  return fin.interrupt(fd) ? -1 : shutdown(fd, how);
                              },
                      });
    }

    void expectData(const test::SocketPair& peer, std::string_view expected) {
        std::array<char, 32> bytes{};
        const auto n = recv(peer.peer.get(), bytes.data(), bytes.size(), 0);
        ASSERT_EQ(ssize_t(expected.size()), n);
        EXPECT_EQ(expected, std::string_view(bytes.data(), expected.size()));
    }

    void expireRetry() {
        clock.advanceTo(clock.now() + 1ms);
        ASSERT_TRUE(expire(clock.now()));
    }

    struct Interruption {
        int fd = -1;
        int remaining = 0;
        int calls = 0;

        bool interrupt(int socket) {
            if (fd != socket) {
                return false;
            }
            ++calls;
            if (remaining == 0) {
                return false;
            }
            --remaining;
            errno = EINTR;
            return true;
        }
    };
    Interruption read, write, fin;
};

TEST_F(InterruptedIoTest, LevelTriggeredIoYieldsAndKeepsTheExactUnsentBytes) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto other_client = test::socketPair();
    auto other_backend = test::socketPair();
    read = {client.local.get(), 3};
    write = {backend.local.get(), 3};
    const auto ids = install(client, backend);
    const auto other = install(other_client, other_backend);
    const std::array batch{detail::ReadyEvent{ids.downstream, EPOLLIN},
                           detail::ReadyEvent{other.upstream, EPOLLIN}};
    ASSERT_EQ(6, send(client.peer.get(), "abcdef", 6, 0));
    for (int i = 1; i <= 3; ++i) {
        ASSERT_EQ(1, send(other_backend.peer.get(), "x", 1, 0));
        ASSERT_TRUE(dispatchBatch(batch));
        EXPECT_EQ(i, read.calls);
        EXPECT_TRUE(interests(ids.downstream).downstream.readable);
        EXPECT_EQ(0, write.calls);
        expectData(other_client, "x");
    }
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_EQ(4, read.calls);
    EXPECT_EQ(1, write.calls);
    EXPECT_TRUE(interests(ids.downstream).upstream.writable);
    for (int i = 2; i <= 4; ++i) {
        ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
        EXPECT_EQ(i, write.calls);
    }
    expectData(backend, "abcdef");
    EXPECT_FALSE(interests(ids.downstream).upstream.writable);
    EXPECT_EQ(0u, bufferBudget().allocated());
    EXPECT_EQ(0u, bufferBudget().reserved());
}

TEST_F(InterruptedIoTest, ResidualReadsAndFinRetryFromTheTimerWithoutANewReadinessEdge) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    read = {client.local.get(), 2};
    fin = {backend.local.get(), 2};
    const auto ids = install(client, backend);
    ASSERT_EQ(8, send(client.peer.get(), "residual", 8, 0));
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_RDWR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLHUP}));
    EXPECT_EQ(1, read.calls);
    EXPECT_EQ(clock.now() + 1ms, nextDeadline());
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLHUP}));
    EXPECT_EQ(1, read.calls);

    expireRetry();
    EXPECT_EQ(2, read.calls);
    EXPECT_EQ(clock.now() + 1ms, nextDeadline());
    expireRetry();
    EXPECT_EQ(3, read.calls);
    EXPECT_TRUE(interests(ids.upstream).upstream.writable);

    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    expectData(backend, "residual");
    EXPECT_EQ(4, read.calls);
    EXPECT_EQ(1, fin.calls);
    EXPECT_EQ(clock.now() + 1ms, nextDeadline());
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_EQ(1, fin.calls);
    char byte;
    EXPECT_EQ(-1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ(EAGAIN, errno);
    expireRetry();
    EXPECT_EQ(2, fin.calls);
    expireRetry();
    EXPECT_EQ(3, fin.calls);
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_FALSE(nextDeadline());
    EXPECT_EQ(0u, sessionCount());
}

} // namespace
} // namespace orbit::proxy
