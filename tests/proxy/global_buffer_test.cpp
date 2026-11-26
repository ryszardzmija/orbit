#include <algorithm>
#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/epoll.h>

#include "reactor_fixture.h"

namespace orbit::proxy {
namespace {

using namespace std::chrono_literals;

class GlobalBufferTest : public ControlledReactorTest {
protected:
    static constexpr size_t block = ResourceConfig::payload_block_size;

    void SetUp() override { setupBudget(); }

    void setupBudget(size_t high = block * 2) {
        createReactor({.idle = 10s}, clock.source(),
                      {
                          .context = this,
                          .receive =
                              [](void* context, int fd, void* data, size_t size, int flags) {
                                  auto& self = *static_cast<GlobalBufferTest*>(context);
                                  EXPECT_GT(size, 0u);
                                  auto& budget = self.bufferBudget();
                                  EXPECT_LE(budget.allocated() + budget.reserved(), block);
                                  self.reads.push_back({fd, size, budget.reserved()});
                                  return recv(fd, data, size, flags);
                              },
                          .send =
                              [](void* context, int fd, const void* data, size_t size, int flags) {
                                  auto& self = *static_cast<GlobalBufferTest*>(context);
                                  if (std::ranges::find(self.blocked, fd) != self.blocked.end()) {
                                      errno = EAGAIN;
                                      return ssize_t{-1};
                                  }
                                  if (auto limit = std::exchange(self.send_limit, std::nullopt)) {
                                      size = std::min(size, *limit);
                                  }
                                  return send(fd, data, size, flags);
                              },
                      },
                      {.buffer_high_watermark = high,
                       .buffer_low_watermark = block / 2,
                       .buffer_budget = block},
                      {
                          .allocation = {this,
                                         [](void* context, detail::AllocationPoint point) noexcept {
                                             return point == static_cast<GlobalBufferTest*>(context)
                                                                 ->failed_allocation;
                                         }},
                      });
    }

    void put(const test::SocketPair& pair, std::string_view payload) {
        ASSERT_EQ(ssize_t(payload.size()),
                  send(pair.peer.get(), payload.data(), payload.size(), 0));
    }

    void expectData(const test::SocketPair& pair, std::string_view payload) {
        std::string data(payload.size(), '\0');
        ASSERT_EQ(ssize_t(data.size()), recv(pair.peer.get(), data.data(), data.size(), 0));
        EXPECT_EQ(payload, data);
    }

    void expectUsage(size_t bytes) {
        EXPECT_EQ(bytes, bufferBudget().allocated());
        EXPECT_EQ(0u, bufferBudget().reserved());
        EXPECT_EQ(block - bytes, bufferBudget().available());
    }

    struct Read {
        int fd;
        size_t size;
        size_t reserved;
    };

    std::optional<detail::AllocationPoint> failed_allocation;
    std::optional<size_t> send_limit;
    std::vector<int> blocked;
    std::vector<Read> reads;
};

TEST_F(GlobalBufferTest, DirectSendCancelsReservationAndPartialSendReusesItsTail) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto ids = install(client, upstream);
    put(client, "direct");
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    ASSERT_EQ(1u, reads.size());
    EXPECT_EQ(block, reads.back().reserved);
    expectData(upstream, "direct");
    expectUsage(0);

    send_limit = 2;
    put(upstream, "abcdef");
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN}));
    EXPECT_EQ(block, reads.back().reserved);
    expectData(client, "ab");
    expectUsage(block);
    put(upstream, "xyz");
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN}));
    EXPECT_EQ(block - 4, reads.back().size);
    EXPECT_EQ(0u, reads.back().reserved);
    EXPECT_EQ(0u, waitingReads());
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLOUT}));
    expectData(client, "cdefxyz");
    expectUsage(0);
}

TEST_F(GlobalBufferTest, RepeatedBlockReleaseServesQueuedDirectionsBeforeActiveReaders) {
    auto a = test::socketPair();
    auto a_dest = test::socketPair();
    auto b = test::socketPair();
    auto b_dest = test::socketPair();
    auto c = test::socketPair();
    auto c_dest = test::socketPair();
    const std::string full(block, 'a');
    const int a_fd = a.local.get();
    const int b_fd = b.local.get();
    const int c_fd = c.local.get();
    blocked = {a_dest.local.get(), b_dest.local.get()};
    const auto first = install(a, a_dest);
    const auto second = install(b_dest, b);
    const auto third = install(c, c_dest);

    put(a, full);
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    expectUsage(block);
    put(b, full);
    put(c, "c");
    ASSERT_TRUE(dispatch({second.upstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({third.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({second.upstream, EPOLLIN}));
    EXPECT_EQ(2u, waitingReads());
    EXPECT_EQ(1u, reads.size());
    EXPECT_FALSE(hasReadyRead());

    blocked.erase(blocked.begin());
    ASSERT_TRUE(dispatch({first.upstream, EPOLLOUT}));
    expectData(a_dest, full);
    expectUsage(0);
    put(a, "will-wait");
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    EXPECT_EQ(3u, waitingReads());
    EXPECT_EQ(1u, reads.size());
    ASSERT_TRUE(resumeReadyRead());
    ASSERT_EQ(2u, reads.size());
    EXPECT_EQ(b_fd, reads.back().fd);
    EXPECT_FALSE(hasReadyRead());
    expectUsage(block);

    blocked.clear();
    ASSERT_TRUE(dispatch({second.downstream, EPOLLOUT}));
    expectData(b_dest, full);
    ASSERT_TRUE(resumeReadyRead());
    EXPECT_EQ(c_fd, reads.back().fd);
    expectData(c_dest, "c");
    ASSERT_TRUE(resumeReadyRead());
    EXPECT_EQ(a_fd, reads.back().fd);
    expectData(a_dest, "will-wait");
    EXPECT_EQ(0u, waitingReads());
    expectUsage(0);
}

TEST_F(GlobalBufferTest, HungUpWaiterResumesWithoutAnEdgeDuringGracefulShutdown) {
    auto a = test::socketPair();
    auto a_dest = test::socketPair();
    auto b = test::socketPair();
    auto b_dest = test::socketPair();
    blocked = {a_dest.local.get()};
    const auto first = install(a, a_dest);
    const auto second = install(b, b_dest);
    put(a, "holds-capacity");
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    put(b, "residual");
    ASSERT_EQ(0, shutdown(b.peer.get(), SHUT_RDWR));
    ASSERT_TRUE(dispatch({second.downstream, EPOLLHUP}));
    EXPECT_FALSE(interests(second.downstream).downstream.readable);
    EXPECT_TRUE(interests(second.downstream).downstream.edge_triggered);
    EXPECT_EQ(1u, waitingReads());
    EXPECT_EQ(1u, reads.size());
    ASSERT_TRUE(shutdownRequest());

    // Idle eviction frees a block. Only the FIFO continuation supplies the next read.
    setIdleAt(first.downstream, clock.now());
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(hasReadyRead());
    ASSERT_TRUE(resumeReadyRead());
    EXPECT_EQ(2u, reads.size());
    EXPECT_EQ(0u, waitingReads());
    expectUsage(block);
    ASSERT_TRUE(dispatch({second.upstream, EPOLLOUT}));
    expectData(b_dest, "residual");
    char byte;
    EXPECT_EQ(0, recv(b_dest.peer.get(), &byte, 1, 0));
    EXPECT_TRUE(stopped());
    expectUsage(0);
}

TEST_F(GlobalBufferTest, LocalWatermarkAndStaleWaitersDoNotStrandReleasedCapacity) {
    setupBudget(block);
    auto a = test::socketPair();
    auto a_dest = test::socketPair();
    auto b = test::socketPair();
    auto b_dest = test::socketPair();
    auto c = test::socketPair();
    auto c_dest = test::socketPair();
    blocked = {a_dest.local.get()};
    const auto first = install(a, a_dest);
    const auto second = install(b, b_dest);
    const auto third = install(c, c_dest);
    const std::string full(block, 'x');
    put(a, full);
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    EXPECT_FALSE(interests(first.downstream).downstream.readable);
    put(b, "b");
    put(c, "c");
    ASSERT_TRUE(dispatch({second.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({third.downstream, EPOLLIN}));
    EXPECT_EQ(2u, waitingReads());
    ASSERT_TRUE(dispatch({second.upstream, EPOLLERR}));
    ASSERT_TRUE(dispatch({second.downstream, EPOLLIN}));
    EXPECT_EQ(1u, waitingReads());
    blocked.clear();
    send_limit = block / 4;
    ASSERT_TRUE(dispatch({first.upstream, EPOLLOUT}));
    EXPECT_FALSE(interests(first.downstream).downstream.readable);
    EXPECT_FALSE(hasReadyRead());
    expectUsage(block);
    ASSERT_TRUE(dispatch({first.upstream, EPOLLOUT}));
    EXPECT_TRUE(interests(first.downstream).downstream.readable);
    ASSERT_TRUE(resumeReadyRead());
    expectData(a_dest, full);
    expectData(c_dest, "c");
    EXPECT_EQ(0u, waitingReads());
    expectUsage(0);
}

TEST_F(GlobalBufferTest, AllocationFailureWithReservationLeavesOtherWaitersRunnable) {
    auto a = test::socketPair();
    auto a_dest = test::socketPair();
    auto b = test::socketPair();
    auto b_dest = test::socketPair();
    auto c = test::socketPair();
    auto c_dest = test::socketPair();
    blocked = {a_dest.local.get(), b_dest.local.get()};
    const auto first = install(a, a_dest);
    const auto second = install(b, b_dest);
    const auto third = install(c, c_dest);
    put(a, "holds-capacity");
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    put(b, "b");
    put(c, "c");
    ASSERT_TRUE(dispatch({second.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({third.downstream, EPOLLIN}));
    ASSERT_TRUE(dispatch({first.upstream, EPOLLERR}));
    failed_allocation = detail::AllocationPoint::PayloadBlock;
    ASSERT_TRUE(resumeReadyRead());
    EXPECT_FALSE(registered(second.downstream));
    expectUsage(0);
    ASSERT_TRUE(hasReadyRead());
    ASSERT_TRUE(resumeReadyRead());
    expectData(c_dest, "c");
    EXPECT_EQ(0u, waitingReads());
    expectUsage(0);
}

TEST_F(GlobalBufferTest, HardShutdownClearsWaitingDirectionsAndCharges) {
    auto a = test::socketPair();
    auto a_dest = test::socketPair();
    auto b = test::socketPair();
    auto b_dest = test::socketPair();
    blocked = {a_dest.local.get()};
    const auto first = install(a, a_dest);
    const auto second = install(b, b_dest);
    put(a, "holds-capacity");
    ASSERT_TRUE(dispatch({first.downstream, EPOLLIN}));
    put(b, "waiting");
    ASSERT_TRUE(dispatch({second.downstream, EPOLLIN}));
    EXPECT_EQ(1u, waitingReads());
    ASSERT_TRUE(shutdownRequest());
    ASSERT_TRUE(shutdownRequest());
    EXPECT_TRUE(stopped());
    EXPECT_EQ(0u, sessionCount());
    EXPECT_EQ(0u, waitingReads());
    expectUsage(0);
}

} // namespace
} // namespace orbit::proxy
