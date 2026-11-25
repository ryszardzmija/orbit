#include <array>
#include <cerrno>
#include <chrono>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "controlled_clock.h"
#include "reactor_fixture.h"

namespace {

enum class IoOperation { Receive, Send };

struct IoObservation {
    int fd;
    int calls = 0;
    ssize_t result = 0;
    int error = 0;
};

class AdvanceClockOnIo;
thread_local AdvanceClockOnIo* observing_io = nullptr;

class AdvanceClockOnIo {
public:
    AdvanceClockOnIo(orbit::proxy::test::ControlledClock& clock,
                     orbit::proxy::detail::Deadline observed_at, IoOperation advance_after,
                     int read_fd, int write_fd)
        : received{read_fd},
          sent{write_fd},
          clock_(clock),
          observed_at_(observed_at),
          advance_after_(advance_after) {
        observing_io = this;
    }

    ~AdvanceClockOnIo() { observing_io = nullptr; }

    void observe(IoOperation operation, int fd, ssize_t result) noexcept {
        auto& observation = operation == IoOperation::Receive ? received : sent;
        if (observation.fd == fd) {
            ++observation.calls;
            observation.result = result;
            observation.error = result < 0 ? errno : 0;
            if (operation == advance_after_) {
                clock_.advanceTo(observed_at_);
            }
        }
    }

    IoObservation received;
    IoObservation sent;

private:
    orbit::proxy::test::ControlledClock& clock_;
    orbit::proxy::detail::Deadline observed_at_;
    IoOperation advance_after_;
};

} // namespace

extern "C" ssize_t __real_recv(int fd, void* buffer, size_t size, int flags);
extern "C" ssize_t __wrap_recv(int fd, void* buffer, size_t size, int flags) {
    auto result = __real_recv(fd, buffer, size, flags);
    if (observing_io) {
        observing_io->observe(IoOperation::Receive, fd, result);
    }
    return result;
}

extern "C" ssize_t __real_send(int fd, const void* buffer, size_t size, int flags);
extern "C" ssize_t __wrap_send(int fd, const void* buffer, size_t size, int flags) {
    auto result = __real_send(fd, buffer, size, flags);
    if (observing_io) {
        observing_io->observe(IoOperation::Send, fd, result);
    }
    return result;
}

namespace orbit::proxy {

using namespace std::chrono_literals;
using detail::DeadlineClock;

class IdleSessionTest : public ProxyReactorTest {
    void SetUp() override { createReactor({.idle = 60s}); }
};

namespace {

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

TEST_F(ProxyReactorTest, IdleExpirationIsDisabledByDefault) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto ids = install(client, backend);
    EXPECT_FALSE(nextDeadline());
    ASSERT_TRUE(expire(detail::Deadline::max()));
    ASSERT_EQ(1, send(client.peer.get(), "d", 1, 0));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('d', byte);
    EXPECT_FALSE(nextDeadline());
}

TEST_F(IdleSessionTest, WouldBlockReadinessAndHalfCloseLeaveInitialIdleDeadlineIntact) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    auto before = DeadlineClock::now();
    auto ids = install(client, backend);
    auto at = nextDeadline();
    ASSERT_TRUE(at);
    EXPECT_GE(*at, before + 60s);
    EXPECT_LE(*at, DeadlineClock::now() + 60s);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLOUT}));
        EXPECT_EQ(at, nextDeadline());
    }
    ASSERT_EQ(0, shutdown(client.peer.get(), SHUT_WR));
    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN | EPOLLRDHUP}));
    char byte;
    EXPECT_EQ(0, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ(at, nextDeadline());
    ASSERT_TRUE(expire(*at - 1ns));
    EXPECT_TRUE(registered(ids.downstream));
    ASSERT_TRUE(expire(*at));
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
    auto previous = DeadlineClock::now() + 5s;
    setIdleAt(busy.downstream, previous);
    setIdleAt(quiet.upstream, previous);
    ASSERT_EQ(1, send(client.peer.get(), "x", 1, 0));
    ASSERT_TRUE(dispatch({busy.downstream, EPOLLIN}));
    ASSERT_TRUE(expire(previous));
    ASSERT_FALSE(registered(quiet.downstream));
    ASSERT_TRUE(registered(busy.upstream));
    char byte;
    ASSERT_EQ(1, recv(backend.peer.get(), &byte, 1, 0));
    EXPECT_EQ('x', byte);

    setIdleAt(busy.upstream, previous);
    ASSERT_EQ(1, send(backend.peer.get(), "y", 1, 0));
    ASSERT_TRUE(dispatch({busy.upstream, EPOLLIN}));
    ASSERT_TRUE(expire(previous));
    ASSERT_TRUE(registered(busy.downstream));
    ASSERT_EQ(1, recv(client.peer.get(), &byte, 1, 0));
    EXPECT_EQ('y', byte);
    ASSERT_TRUE(nextDeadline());
    EXPECT_GT(*nextDeadline(), previous);
}

TEST_F(IdleSessionTest, QueuedWriteRefreshesOnlyOnSuccessfulSendAndPausedSessionCanExpire) {
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
    auto previous = DeadlineClock::now() + 5s;
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
    setIdleAt(ids.downstream, DeadlineClock::now());
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

    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.upstream, at);
    test::ControlledClock clock(at - 1ns);
    AdvanceClockOnIo observation(clock, at, IoOperation::Receive, backend_fd, backend_fd);
    ASSERT_TRUE(dispatch({ids.upstream, EPOLLIN | EPOLLOUT}));
    EXPECT_EQ(1, observation.received.calls);
    EXPECT_EQ(-1, observation.received.result);
    EXPECT_EQ(EAGAIN, observation.received.error);
    EXPECT_EQ(0, observation.sent.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
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
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.downstream, at);
    test::ControlledClock clock(at - 1ns);
    AdvanceClockOnIo observation(clock, at, IoOperation::Receive, client_fd, backend_fd);

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
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.downstream, at);
    test::ControlledClock clock(at - 2ns);
    AdvanceClockOnIo observation(clock, at - 1ns, IoOperation::Receive, client_fd, backend_fd);

    ASSERT_TRUE(dispatch({ids.downstream, EPOLLIN}));
    EXPECT_EQ(1, observation.received.result);
    EXPECT_EQ(1, observation.sent.result);
    EXPECT_EQ(at - 1ns + 60s, nextDeadline());
    ASSERT_TRUE(expire(at));
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
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.upstream, at);
    test::ControlledClock clock(at - 1ns);
    AdvanceClockOnIo observation(clock, at, IoOperation::Send, source_fd, destination_fd);

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
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.upstream, at);
    test::ControlledClock clock(at - 1ns);
    AdvanceClockOnIo observation(clock, at, IoOperation::Send, source_fd, destination_fd);

    ASSERT_TRUE(dispatch({ids.upstream, EPOLLOUT}));
    EXPECT_EQ(1, observation.sent.calls);
    EXPECT_EQ(-1, observation.sent.result);
    EXPECT_EQ(EAGAIN, observation.sent.error);
    EXPECT_EQ(0, observation.received.calls);
    EXPECT_FALSE(registered(ids.downstream));
    EXPECT_FALSE(registered(ids.upstream));
    EXPECT_FALSE(nextDeadline());
}

TEST_F(IdleSessionTest, RemovedSessionDeadlineCannotCloseReplacement) {
    auto client = test::socketPair();
    auto backend = test::socketPair();
    int old_fd = client.local.get();
    auto old = install(client, backend);
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(old.downstream, at);
    ASSERT_TRUE(dispatch({old.downstream, EPOLLERR}));
    ASSERT_FALSE(nextDeadline());
    auto replacement = test::socketPair();
    auto new_backend = test::socketPair();
    ASSERT_EQ(old_fd, replacement.local.get());
    auto live = install(replacement, new_backend);
    ASSERT_TRUE(expire(at));
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
    auto at = DeadlineClock::now() + 5s;
    setIdleAt(ids.upstream, at);
    ASSERT_TRUE(shutdownRequest());
    EXPECT_FALSE(stopped());
    ASSERT_TRUE(expire(at));
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(nextDeadline());
    EXPECT_FALSE(registered(ids.upstream));
}

TEST_F(IdleSessionTest, DueGracefulShutdownTakesPrecedenceOverAnIdleBurst) {
    auto idle_at = DeadlineClock::now() + 5s;
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
    ASSERT_TRUE(expire(DeadlineClock::now() + 30s));
    EXPECT_TRUE(stopped());
    EXPECT_FALSE(registered(longer.downstream));
    EXPECT_FALSE(nextDeadline());
    char byte;
    EXPECT_EQ(0, recv(longer_client.peer.get(), &byte, 1, 0));
}

} // namespace orbit::proxy
