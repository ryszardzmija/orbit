#pragma once

#include <cerrno>
#include <chrono>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>

#include "controlled_clock.h"
#include "proxy/proxy.h"
#include "socket_pair.h"

namespace orbit::proxy {

// Drive the same one-event dispatcher used by start(), with a controlled batch order.
class ProxyReactorTest : public testing::Test {
protected:
    void SetUp() override { createReactor({}); }

    // Clock and socket callback contexts can belong to derived fixtures.
    void TearDown() override { reactor.reset(); }

    void createReactor(TimeoutConfig timeouts, SteadyTimeSource time_source = {},
                       net::StreamOperations stream_operations = {}, ResourceConfig resources = {},
                       ReactorOperations operations = {}) {
        operations.stream = stream_operations;
        auto result = ProxyReactor::createImpl({"127.0.0.1", 0}, {"127.0.0.1", 1}, timeouts,
                                               resources, time_source, operations);
        ASSERT_TRUE(result) << result.error().message;
        reactor.emplace(std::move(*result));
    }

    detail::SessionSourceIds install(test::SocketPair& downstream, test::SocketPair& upstream) {
        ProxyReactor::AdmissionReservation admission(reactor->transient_admission_);
        auto added = reactor->addSession(std::move(upstream.local), std::move(downstream.local),
                                         detail::Deadline::max(), admission);
        if (!added) {
            throw std::system_error(added.error().code);
        }
        synchronizeAdmission();
        return reactor->session_manager_.sourceIds(*added);
    }

    auto dispatch(detail::ReadyEvent event) { return dispatchBatch({&event, 1}); }
    Status<FatalReactorError> dispatchBatch(std::span<const detail::ReadyEvent> events) {
        return reactor->dispatchBatch(events);
    }
    bool registered(detail::SourceId id) const { return reactor->sources_.find(id).has_value(); }
    bool watching(detail::SourceId id) const { return reactor->poller_.isWatching(id); }

    void setUpstreamDialer(detail::UpstreamDialer dialer) {
        reactor->upstream_dialer_ = std::move(dialer);
    }

    detail::SourceId installPending(test::SocketPair& client, test::SocketPair& attempt,
                                    std::optional<detail::Deadline> at = std::nullopt) {
        return installPending({
            .accepted_connection_fd = std::move(client.local),
            .accepted_address = {},
            .attempted_connection_fd = std::move(attempt.local),
            .attempted_address_idx = 0,
            .deadline = at ? *at : reactor->time_source_.now() + std::chrono::seconds(10),
        });
    }

    detail::SourceId installPending(detail::PendingConnection pending) {
        ProxyReactor::AdmissionReservation admission(reactor->transient_admission_);
        auto added = reactor->registerPendingConnection(std::move(pending), admission);
        if (!added) {
            throw std::system_error(added.error().code);
        }
        synchronizeAdmission();
        return *added;
    }

    auto shutdownRequest() { return reactor->handleShutdownRequest(); }
    auto listenerId() const { return reactor->active_listener_->listener_id; }
    bool listenerAccepting() const {
        return reactor->active_listener_ && reactor->active_listener_->read_enabled;
    }
    size_t pendingCount() const { return reactor->pendingConnectionCount(); }
    size_t sessionCount() const { return reactor->session_manager_.size(); }
    size_t totalCount() const { return pendingCount() + sessionCount(); }
    bool canAdmit() const { return reactor->canAdmit(); }
    auto extractPending(detail::SourceId id) { return reactor->takePending(id); }
    FileDescriptor connectToListener() const {
        const auto& listener = reactor->active_listener_->listener;
        const auto& address = listener.localAddress();
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd == -1) {
            throw std::system_error(errno, std::system_category());
        }
        FileDescriptor client(fd);
        if (connect(client.get(), reinterpret_cast<const sockaddr*>(&address.addr),
                    address.addrlen) != 0) {
            if (errno != EINPROGRESS) {
                throw std::system_error(errno, std::system_category());
            }
            pollfd ready{client.get(), POLLOUT, 0};
            if (poll(&ready, 1, 1000) != 1) {
                throw std::runtime_error("Client did not connect to the test listener");
            }
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(client.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
                error = errno;
            }
            if (error != 0) {
                throw std::system_error(error, std::system_category());
            }
        }
        return client;
    }
    bool stopped() const { return reactor->shouldStop(); }
    int timerFd() const { return reactor->timer_fd_.get(); }
    auto handleTimer() {
        auto result = reactor->handleTimer({});
        if (result) {
            synchronizeAdmission();
        }
        return result;
    }

    detail::SessionId sessionFor(detail::SourceId source) const {
        return std::get<detail::EndpointRegistration>(reactor->sources_.find(source).value())
            .session_id;
    }
    auto interests(detail::SourceId source) const {
        return reactor->session_manager_.interests(sessionFor(source));
    }
    void setIdleAt(detail::SourceId source, detail::Deadline at) {
        reactor->deadlines_.schedule({detail::DeadlineKind::Idle, sessionFor(source)}, at);
    }
    auto currentSessionSources() const {
        auto ids = reactor->session_manager_.ids();
        if (ids.empty()) {
            throw std::runtime_error("Expected an established session");
        }
        return reactor->session_manager_.sourceIds(*ids.begin());
    }
    auto dialResult(detail::UpstreamDialResult result) {
        ProxyReactor::AdmissionReservation admission(reactor->transient_admission_);
        auto status = reactor->handleDialResult(std::move(result), admission);
        if (status) {
            synchronizeAdmission();
        }
        return status;
    }
    auto nextDeadline() const { return reactor->deadlines_.next(); }
    auto nextWakeup() const { return reactor->nextWakeup(); }
    auto retryAt() const { return reactor->admission_retry_at_; }
    auto expire(detail::Deadline now) {
        auto result = reactor->expireDeadlines(now);
        if (result) {
            if (auto synced = reactor->synchronizeListener(); !synced) {
                return Status<FatalReactorError>(
                    std::unexpected(FatalReactorError{synced.error().message()}));
            }
        }
        return result;
    }
    void shortenGrace() {
        reactor->deadlines_.schedule({detail::DeadlineKind::Shutdown, 0},
                                     detail::DeadlineClock::now() + std::chrono::milliseconds(1));
    }
    auto arm() { return reactor->synchronizeTimer(); }
    auto waitForEvents(bool nonblocking = false) { return reactor->poller_.wait(nonblocking); }
    auto resumeReadyRead() { return reactor->resumeReadyRead(); }
    auto resumeDeferredDials() { return reactor->resumeDeferredDials(); }
    auto deferredDialCount() const {
        return reactor->deferred_attempts_.size() + reactor->deferred_completions_.size();
    }
    bool hasRunnableDials() const { return reactor->hasRunnableDials(); }
    auto runTurn() { return reactor->runTurn(); }
    bool hasReadyRead() const { return reactor->session_manager_.hasReadyRead(); }
    auto waitingReads() const { return reactor->session_manager_.waitingReadCount(); }
    const detail::BufferBudget& bufferBudget() const {
        return reactor->session_manager_.bufferBudget();
    }

    std::optional<ProxyReactor> reactor;

private:
    void synchronizeAdmission() {
        if (auto result = reactor->synchronizeListener(); !result) {
            throw std::system_error(result.error());
        }
    }
};

class ControlledReactorTest : public ProxyReactorTest {
protected:
    void SetUp() override { createReactor({}, clock.source()); }

    test::ControlledClock clock;
};

} // namespace orbit::proxy
