#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <vector>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "reactor_fixture.h"

namespace orbit::proxy {

using namespace std::chrono_literals;

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

TEST_F(ProxyReactorTest, ExpiredBudgetBeatsAlreadyWritableCompletion) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    auto pending = installPending(client, attempt, detail::DeadlineClock::now());
    // SO_ERROR on this connected socket is zero. Its copied completion precedes the timer.
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_FALSE(registered(pending));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(attempt.peer.get(), &byte, 1, 0));
}

TEST_F(ProxyReactorTest, SuccessfulPromotionCancelsDialDeadlineAndRetainsHalfClosedRequest) {
    auto client = test::socketPair();
    auto attempt = test::socketPair();
    ASSERT_EQ(1, send(client.peer.get(), "q", 1, 0));
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    auto pending = installPending(client, attempt);
    ASSERT_TRUE(nextDeadline());
    ASSERT_TRUE(dispatch({pending, EPOLLOUT}));
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(pending));
    ASSERT_TRUE(expire(detail::DeadlineClock::now() + 20s));
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

TEST_F(ProxyReactorTest, ExpiredConnectedHandoffReleasesBothRegisteredEndpoints) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    ASSERT_TRUE(dialResult(detail::UpstreamDialConnected{
        std::move(client.local), std::move(backend.local), {}, {}, detail::DeadlineClock::now()}));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    ASSERT_TRUE(shutdownRequest());
    EXPECT_TRUE(stopped());
}

TEST_F(ProxyReactorTest, RegistrationFailureReleasesClientAndDialDeadline) {
    auto waiting = test::socketPair();
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto at = detail::DeadlineClock::now() + 10s;
    ASSERT_TRUE(dialResult(detail::UpstreamDialInProgress{
        {std::move(waiting.local), {}, FileDescriptor(fd), 0, at, {}}}));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(waiting.peer.get(), &byte, 1, 0));
}

TEST_F(ProxyReactorTest, ExpirationBurstLeavesDueWorkArmedForAnotherTurn) {
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

} // namespace orbit::proxy
