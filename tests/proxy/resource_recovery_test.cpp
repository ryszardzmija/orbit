#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "reactor_fixture.h"

namespace orbit::proxy {
namespace {

using namespace std::chrono_literals;
using detail::AllocationPoint;

class ResourceRecoveryTest : public ControlledReactorTest {
protected:
    void SetUp() override {
        createReactor(
            {.idle = 10s}, clock.source(),
            {
                .context = this,
                .send =
                    [](void* context, int fd, const void* data, size_t size, int flags) {
                        auto& self = *static_cast<ResourceRecoveryTest*>(context);
                        if (fd == self.blocked_send_fd) {
                            errno = EAGAIN;
                            return ssize_t{-1};
                        }
                        return send(fd, data, size, flags);
                    },
            },
            {.max_connections = 2, .max_pending_connections = 1},
            {
                .accept = {this,
                           [](void* context, int fd, sockaddr* addr, socklen_t* len, int flags) {
                               auto& self = *static_cast<ResourceRecoveryTest*>(context);
                               ++self.accept_calls;
                               if (self.accept_error != 0) {
                                   errno = self.accept_error;
                                   return -1;
                               }
                               return accept4(fd, addr, len, flags);
                           }},
                .epoll = {this,
                          [](void* context, int epfd, int op, int fd, epoll_event* event) {
                              auto& self = *static_cast<ResourceRecoveryTest*>(context);
                              if (op == EPOLL_CTL_ADD) {
                                  self.previous_add = self.last_add;
                                  self.last_add = event->data.u64;
                              }
                              if (op == self.failed_op && self.ops_before_failure-- == 0) {
                                  errno = self.epoll_error;
                                  return -1;
                              }
                              return epoll_ctl(epfd, op, fd, event);
                          }},
                .allocation = {this,
                               [](void* context, AllocationPoint point) noexcept {
                                   auto& self = *static_cast<ResourceRecoveryTest*>(context);
                                   return point == self.failed_allocation &&
                                          self.allocations_before_failure-- == 0;
                               }},
            });
    }

    void advanceToRetry() {
        ASSERT_TRUE(retryAt());
        clock.advanceTo(*retryAt());
        ASSERT_TRUE(expire(clock.now()));
    }

    auto connected(test::SocketPair& downstream, test::SocketPair& upstream) {
        return dialResult(detail::UpstreamDialConnected{
            .accepted_connection_fd = std::move(downstream.local),
            .upstream_connection_fd = std::move(upstream.local),
            .accepted_address = {},
            .upstream_address = {},
            .deadline = clock.now() + 1s,
        });
    }

    void expectForwarding(const test::SocketPair& source, const test::SocketPair& destination,
                          detail::SourceId source_id) {
        ASSERT_EQ(1, send(source.peer.get(), "r", 1, 0));
        ASSERT_TRUE(dispatch({source_id, EPOLLIN}));
        char byte;
        ASSERT_EQ(1, recv(destination.peer.get(), &byte, 1, 0));
        EXPECT_EQ('r', byte);
    }

    void expectClosed(const test::SocketPair& pair) {
        char byte;
        EXPECT_EQ(0, recv(pair.peer.get(), &byte, 1, 0));
    }

    int accept_error = 0;
    int accept_calls = 0;
    int blocked_send_fd = -1;
    int failed_op = -1;
    int epoll_error = ENOSPC;
    int ops_before_failure = 0;
    detail::SourceId previous_add = 0;
    detail::SourceId last_add = 0;
    std::optional<AllocationPoint> failed_allocation;
    int allocations_before_failure = 0;
};

TEST_F(ResourceRecoveryTest, PersistentAcceptPressureRetriesOnlyOnTheSharedTimer) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    const auto listener = listenerId();
    int attempts = 0;
    for (auto error : {EMFILE, ENFILE, ENOMEM, ENOBUFS}) {
        accept_error = error;
        ASSERT_TRUE(dispatch({listener, EPOLLIN}));
        EXPECT_EQ(++attempts, accept_calls);
        EXPECT_FALSE(listenerAccepting());
        EXPECT_EQ(0u, pendingCount());
        ASSERT_TRUE(retryAt());
        EXPECT_EQ(retryAt(), nextWakeup());
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(dispatch({listener, EPOLLIN}));
        }
        clock.advanceTo(*retryAt() - 1ns);
        ASSERT_TRUE(expire(clock.now()));
        EXPECT_EQ(attempts, accept_calls);
        EXPECT_FALSE(listenerAccepting());
        expectForwarding(client, upstream, live.downstream);
        advanceToRetry();
        EXPECT_TRUE(listenerAccepting());
    }
    accept_error = 0;
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    EXPECT_EQ(++attempts, accept_calls);
    EXPECT_FALSE(retryAt());
}

TEST_F(ResourceRecoveryTest, PendingCapacityAndPressureClearIndependentlyAndShutdownWins) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    const auto listener = listenerId();
    accept_error = EMFILE;
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    auto waiting = test::socketPair();
    auto attempt = test::socketPair();
    installPending(waiting, attempt, clock.now() + 1s);
    advanceToRetry();
    EXPECT_FALSE(listenerAccepting());
    ASSERT_TRUE(dispatch({live.upstream, EPOLLERR}));
    EXPECT_FALSE(listenerAccepting()); // The pending slot is still occupied.
    clock.advanceTo(clock.now() + 1s);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(listenerAccepting());

    auto drain_client = test::socketPair();
    auto drain_backend = test::socketPair();
    const auto drain = install(drain_client, drain_backend);
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    auto still_pending = test::socketPair();
    auto pending_fd = test::socketPair();
    const auto pending = installPending(still_pending, pending_fd, clock.now() + 1ms);
    clock.advanceTo(clock.now() + 1ms);
    ASSERT_TRUE(expire(clock.now()));
    EXPECT_FALSE(registered(pending));
    EXPECT_FALSE(listenerAccepting()); // Released slots cannot bypass the cooldown.
    const auto cooldown = retryAt();
    ASSERT_TRUE(cooldown);
    ASSERT_TRUE(shutdownRequest());
    EXPECT_FALSE(retryAt());
    clock.advanceTo(*cooldown);
    ASSERT_TRUE(expire(clock.now()));
    ASSERT_TRUE(dispatch({listener, EPOLLIN}));
    EXPECT_EQ(2, accept_calls);
    EXPECT_FALSE(listenerAccepting());
    expectForwarding(drain_client, drain_backend, drain.downstream);
}

TEST_F(ResourceRecoveryTest, EndpointAddPressureRollsBackFirstWatchAndPendingAddLeavesNoDeadline) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    failed_op = EPOLL_CTL_ADD;
    ops_before_failure = 1;
    auto failed_client = test::socketPair();
    auto failed_peer = test::socketPair();
    ASSERT_TRUE(connected(failed_client, failed_peer));
    for (auto id : {previous_add, last_add}) {
        EXPECT_FALSE(registered(id));
        EXPECT_FALSE(watching(id));
    }
    EXPECT_EQ(1u, sessionCount());
    EXPECT_EQ(0u, pendingCount());
    EXPECT_FALSE(listenerAccepting());
    expectClosed(failed_client);
    expectClosed(failed_peer);
    advanceToRetry();

    ops_before_failure = 0;
    auto waiting = test::socketPair();
    auto dial = test::socketPair();
    const auto idle = nextDeadline();
    ASSERT_TRUE(dialResult(detail::UpstreamDialInProgress{
        {std::move(waiting.local), {}, std::move(dial.local), 0, clock.now() + 1s}}));
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(idle, nextDeadline());
    EXPECT_FALSE(registered(last_add));
    EXPECT_FALSE(watching(last_add));
    expectClosed(waiting);
    expectClosed(dial);
    expectForwarding(client, upstream, live.downstream);
}

TEST_F(ResourceRecoveryTest, SetupAllocationFailuresReleaseOwnersAndBothDeadlineIndexes) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    const auto initial_deadline = nextDeadline();
    for (auto point : {AllocationPoint::SendBuffer, AllocationPoint::DeadlineIndex}) {
        failed_allocation = point;
        allocations_before_failure = point == AllocationPoint::SendBuffer ? 1 : 0;
        const auto before = last_add;
        auto failed_client = test::socketPair();
        auto failed_backend = test::socketPair();
        ASSERT_TRUE(connected(failed_client, failed_backend));
        if (point == AllocationPoint::SendBuffer) {
            EXPECT_EQ(before, last_add);
        } else {
            EXPECT_FALSE(registered(previous_add));
            EXPECT_FALSE(registered(last_add));
        }
        EXPECT_EQ(initial_deadline, nextDeadline());
        EXPECT_EQ(1u, sessionCount());
        EXPECT_FALSE(listenerAccepting());
        expectClosed(failed_client);
        expectClosed(failed_backend);
        advanceToRetry();
    }
    allocations_before_failure = 0;
    auto waiting = test::socketPair();
    auto candidate = test::socketPair();
    ASSERT_TRUE(dialResult(detail::UpstreamDialInProgress{
        {std::move(waiting.local), {}, std::move(candidate.local), 0, clock.now() + 1s}}));
    EXPECT_EQ(0u, pendingCount());
    EXPECT_EQ(initial_deadline, nextDeadline());
    EXPECT_FALSE(registered(last_add));
    EXPECT_FALSE(watching(last_add));
    expectClosed(waiting);
    expectClosed(candidate);
    advanceToRetry();
    failed_allocation.reset();
    auto recovered_client = test::socketPair();
    auto recovered_backend = test::socketPair();
    ASSERT_TRUE(connected(recovered_client, recovered_backend));
    EXPECT_EQ(2u, sessionCount());
    expectForwarding(client, upstream, live.downstream);
}

TEST_F(ResourceRecoveryTest, PayloadAllocationFailureClosesOnlyItsSession) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    auto failed_client = test::socketPair();
    auto failed_backend = test::socketPair();
    blocked_send_fd = failed_backend.local.get();
    const auto failed = install(failed_client, failed_backend);
    std::array<char, 4096> bytes{};
    ASSERT_EQ(ssize_t(bytes.size()), send(failed_client.peer.get(), bytes.data(), bytes.size(), 0));
    ASSERT_TRUE(dispatch({failed.downstream, EPOLLIN}));
    EXPECT_TRUE(interests(failed.downstream).upstream.writable);
    failed_allocation = AllocationPoint::PayloadBlock;
    ASSERT_EQ(1, send(failed_client.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch({failed.downstream, EPOLLIN}));
    EXPECT_FALSE(registered(failed.downstream));
    EXPECT_FALSE(registered(failed.upstream));
    EXPECT_EQ(1u, sessionCount());
    EXPECT_TRUE(listenerAccepting());
    expectClosed(failed_client);
    expectClosed(failed_backend);
    expectForwarding(client, upstream, live.downstream);
}

TEST_F(ResourceRecoveryTest, RecoverableModFailureClosesAffectedPairAndOtherFailuresStayFatal) {
    auto client = test::socketPair();
    auto upstream = test::socketPair();
    const auto live = install(client, upstream);
    for (auto error : {ENOENT, EBADF}) {
        auto failed_client = test::socketPair();
        auto failed_backend = test::socketPair();
        const auto failed = install(failed_client, failed_backend);
        failed_op = EPOLL_CTL_MOD;
        epoll_error = error;
        ops_before_failure = 0;
        auto result = dispatch({failed.downstream, EPOLLRDHUP});
        EXPECT_EQ(error == ENOENT, result.has_value());
        EXPECT_FALSE(registered(failed.downstream));
        EXPECT_FALSE(registered(failed.upstream));
        expectClosed(failed_client);
        expectClosed(failed_backend);
        expectForwarding(client, upstream, live.downstream);
    }
}

} // namespace
} // namespace orbit::proxy
