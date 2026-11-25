#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "proxy/proxy.h"
#include "socket_pair.h"

namespace orbit::proxy {

// Drive the same one-event dispatcher used by start(), with a controlled batch order.
class ProxyReactorTest : public testing::Test {
protected:
    void SetUp() override { createReactor({}); }

    void createReactor(TimeoutConfig timeouts) {
        auto result = ProxyReactor::create({"127.0.0.1", 0}, {"127.0.0.1", 1}, timeouts);
        ASSERT_TRUE(result) << result.error().message;
        reactor.emplace(std::move(*result));
    }

    detail::SessionSourceIds install(test::SocketPair& downstream, test::SocketPair& upstream) {
        auto added = reactor->addSession(std::move(upstream.local), std::move(downstream.local),
                                         detail::Deadline::max());
        if (!added) {
            throw std::runtime_error(added.error().message);
        }
        return reactor->session_manager_.sourceIds(*added);
    }

    auto dispatch(detail::ReadyEvent event) { return reactor->dispatch(event); }
    bool registered(detail::SourceId id) const { return reactor->sources_.find(id).has_value(); }

    detail::SourceId installPending(test::SocketPair& client, test::SocketPair& attempt,
                                    detail::Deadline at = detail::DeadlineClock::now() +
                                                          std::chrono::seconds(10)) {
        auto added = reactor->registerPendingConnection({
            .accepted_connection_fd = std::move(client.local),
            .accepted_address = {},
            .attempted_connection_fd = std::move(attempt.local),
            .attempted_address_idx = 0,
            .deadline = at,
        });
        if (!added) {
            throw std::system_error(added.error());
        }
        return *added;
    }

    auto shutdownRequest() { return reactor->handleShutdownRequest(); }
    auto listenerId() const { return reactor->active_listener_->listener_id; }
    bool stopped() const { return reactor->shouldStop(); }
    int timerFd() const { return reactor->timer_fd_.get(); }
    auto handleTimer() { return reactor->handleTimer({}); }

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
        return reactor->handleDialResult(std::move(result));
    }
    auto nextDeadline() const { return reactor->deadlines_.next(); }
    auto expire(detail::Deadline now) { return reactor->expireDeadlines(now); }
    void shortenGrace() {
        reactor->deadlines_.schedule({detail::DeadlineKind::Shutdown, 0},
                                     detail::DeadlineClock::now() + std::chrono::milliseconds(1));
    }
    auto arm() { return reactor->synchronizeTimer(); }
    auto waitForEvents() { return reactor->poller_.wait(); }

    std::optional<ProxyReactor> reactor;
};

} // namespace orbit::proxy
