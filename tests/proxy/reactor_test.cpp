#include <cerrno>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include "proxy/proxy.h"
#include "socket_pair.h"

namespace orbit::proxy {

// Drive the same one-event dispatcher used by start(), with a controlled batch order.
class ProxyReactorTest : public testing::Test {
protected:
    void SetUp() override {
        auto result = ProxyReactor::create({"127.0.0.1", 0}, {"127.0.0.1", 1});
        ASSERT_TRUE(result) << result.error().message;
        reactor.emplace(std::move(*result));
    }

    detail::SessionSourceIds install(test::SocketPair& downstream, test::SocketPair& upstream) {
        auto added = reactor->addSession(std::move(upstream.local), std::move(downstream.local));
        if (!added) {
            throw std::runtime_error(added.error().message);
        }
        return reactor->session_manager_.sourceIds(*added);
    }

    auto dispatch(detail::ReadyEvent event) { return reactor->dispatch(event); }
    bool registered(detail::SourceId id) const { return reactor->sources_.find(id).has_value(); }

    detail::SourceId installPending(test::SocketPair& client, test::SocketPair& attempt) {
        auto added = reactor->registerPendingConnection({
            .accepted_connection_fd = std::move(client.local),
            .accepted_address = {},
            .attempted_connection_fd = std::move(attempt.local),
            .attempted_address_idx = 0,
        });
        if (!added) {
            throw std::system_error(added.error());
        }
        return *added;
    }

    auto shutdownRequest() { return reactor->handleShutdownRequest(); }
    auto listenerId() const { return reactor->active_listener_->listener_id; }
    bool stopped() const { return reactor->shouldStop(); }
    int timerFd() const { return reactor->shutdown_timer_fd_.get(); }
    auto handleTimer() { return reactor->handleShutdownTimer({}); }

    std::optional<ProxyReactor> reactor;
};

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
    itimerspec soon{};
    soon.it_value.tv_nsec = 1;
    ASSERT_EQ(0, timerfd_settime(timerFd(), 0, &soon, nullptr));
    pollfd timer{timerFd(), POLLIN, 0};
    ASSERT_EQ(1, poll(&timer, 1, 1000));
    ASSERT_TRUE(handleTimer());
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
}

} // namespace orbit::proxy
