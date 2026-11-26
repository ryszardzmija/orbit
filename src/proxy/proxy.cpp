#include "proxy/proxy.h"

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

#include <cerrno>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>

#include "common/fd.h"
#include "net/address_format.h"
#include "proxy/detail/epoll/epoll_poller.h"
#include "proxy/detail/session/session_manager.h"
#include "proxy/detail/signal_fd.h"
#include "proxy/detail/timer_fd.h"

namespace orbit::proxy {

namespace {

constexpr detail::DeadlineKey shutdown_deadline{detail::DeadlineKind::Shutdown, 0};

// Lets a set of lambdas be combined into a single callable for std::visit.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

bool isRecoverableAcceptError(const std::error_code& error) {
    switch (error.value()) {
    case ECONNABORTED:
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
    case EPERM:
        return true;
    default:
        return false;
    }
}

uint32_t getEventMask(const detail::EndpointInterests& interests) {
    uint32_t event_mask = interests.edge_triggered ? EPOLLET : 0;
    if (interests.readable) {
        event_mask |= EPOLLIN;
    }
    if (interests.writable) {
        event_mask |= EPOLLOUT;
    }
    if (interests.peer_half_close) {
        event_mask |= EPOLLRDHUP;
    }
    return event_mask;
}

} // namespace

ProxyReactor::ProxyReactor(detail::EpollPoller poller, FileDescriptor shutdown_signal_fd,
                           FileDescriptor timer_fd, detail::UpstreamDialer upstream_dialer,
                           TimeoutConfig timeouts, ResourceConfig resources,
                           SteadyTimeSource time_source, net::StreamOperations stream_operations)
    : poller_(std::move(poller)),
      session_manager_(
          detail::SessionManagerOptions{
              .send_buffer =
                  {
                      .block_size = ResourceConfig::payload_block_size,
                      .high_watermark = resources.buffer_high_watermark,
                      .low_watermark = resources.buffer_low_watermark,
                  },
              .forwarder_buffer_capacity = forwarder_buf_cap,
              .sender_buffer_capacity = sender_buf_cap,
          },
          stream_operations),
      shutdown_signal_fd_(std::move(shutdown_signal_fd)),
      timer_fd_(std::move(timer_fd)),
      timeouts_(timeouts),
      time_source_(time_source),
      upstream_dialer_(std::move(upstream_dialer)) {}

Result<ProxyReactor, ProxyCreateError>
ProxyReactor::create(const net::ListenSocketAddress& listen_address,
                     const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts,
                     ResourceConfig resources) {
    return createImpl(listen_address, upstream_address, timeouts, resources, {}, {});
}

Result<ProxyReactor, ProxyCreateError>
ProxyReactor::createImpl(const net::ListenSocketAddress& listen_address,
                         const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts,
                         ResourceConfig resources, SteadyTimeSource time_source,
                         net::StreamOperations stream_operations) {
    if (timeouts.connect.count() < 1 || timeouts.connect > TimeoutConfig::maximum) {
        return std::unexpected(ProxyCreateError{"Invalid connection timeout"});
    }
    if (timeouts.idle.count() < 0 || timeouts.idle > TimeoutConfig::maximum) {
        return std::unexpected(ProxyCreateError{"Invalid idle timeout"});
    }
    if (!resources.validBufferWatermarks()) {
        return std::unexpected(ProxyCreateError{"Invalid directional buffer watermarks"});
    }
    auto poller_create_result = detail::EpollPoller::create();
    if (!poller_create_result) {
        return std::unexpected(ProxyCreateError{poller_create_result.error().message()});
    }
    detail::EpollPoller poller = std::move(*poller_create_result);

    auto signalfd_create_result = detail::createShutdownSignalFd();
    if (!signalfd_create_result) {
        return std::unexpected(ProxyCreateError{signalfd_create_result.error().message()});
    }
    FileDescriptor signal_fd = std::move(signalfd_create_result.value());

    auto timerfd_create_result = detail::createTimerFd();
    if (!timerfd_create_result) {
        return std::unexpected(ProxyCreateError{timerfd_create_result.error().message()});
    }
    FileDescriptor timer_fd = std::move(timerfd_create_result.value());

    auto dialer_create_result = detail::UpstreamDialer::create(upstream_address);
    if (!dialer_create_result) {
        return std::unexpected(ProxyCreateError{dialer_create_result.error().message});
    }
    detail::UpstreamDialer dialer = std::move(dialer_create_result.value());

    auto listener_create_result = net::Listener::create(listen_address, max_backlog_size);
    if (!listener_create_result) {
        return std::unexpected(ProxyCreateError{listener_create_result.error().message});
    }
    net::Listener listener = std::move(listener_create_result.value());

    ProxyReactor reactor(std::move(poller), std::move(signal_fd), std::move(timer_fd),
                         std::move(dialer), timeouts, resources, time_source, stream_operations);

    auto listener_register_result = reactor.registerListener(listener.fd());
    if (!listener_register_result) {
        return std::unexpected(ProxyCreateError{listener_register_result.error().message()});
    }
    detail::SourceId listener_id = listener_register_result.value();

    reactor.active_listener_ = detail::ActiveListener{
        .listener = std::move(listener),
        .listener_id = listener_id,
    };

    if (auto register_result = reactor.registerShutdownSignalEvent(); !register_result) {
        return std::unexpected(ProxyCreateError{register_result.error().message()});
    }

    if (auto register_result = reactor.registerTimerEvent(); !register_result) {
        return std::unexpected(ProxyCreateError{register_result.error().message()});
    }

    return std::move(reactor);
}

Status<ProxyRuntimeError> ProxyReactor::start() {
    spdlog::info("Starting proxying traffic...");

    while (!shouldStop()) {
        if (auto armed = synchronizeTimer(); !armed) {
            return std::unexpected(ProxyRuntimeError{armed.error().message()});
        }
        auto wait_result = poller_.wait();
        if (!wait_result) {
            return std::unexpected(ProxyRuntimeError{wait_result.error().message()});
        }

        for (const detail::ReadyEvent& event : *wait_result) {
            auto handler_result = dispatch(event);
            if (!handler_result) {
                spdlog::error("Error in reactor event handler: {}", handler_result.error().message);
                return std::unexpected(ProxyRuntimeError{handler_result.error().message});
            }

            if (shouldStop()) {
                break;
            }
        }
    }

    if (auto disarmed = synchronizeTimer(); !disarmed) {
        return std::unexpected(ProxyRuntimeError{disarmed.error().message()});
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::dispatch(detail::ReadyEvent event) {
    auto registration = sources_.find(event.source_id);

    if (!registration) {
        return {};
    }

    return std::visit(
        overloaded{
            [&](detail::EndpointRegistration r) { return handleEndpoint(r, event.events); },
            [&](detail::ListenerRegistration r) { return handleListener(r); },
            [&](detail::ShutdownSignalRegistration r) { return handleShutdownSignal(r); },
            [&](detail::TimerRegistration r) { return handleTimer(r); },
            [&](detail::PendingDialRegistration) {
                return handlePendingConnection(event.source_id);
            },
        },
        *registration);
}

Status<std::error_code> ProxyReactor::closeSession(detail::SessionId session_id) {
    if (!session_manager_.contains(session_id)) {
        return {};
    }

    if (timeouts_.idle.count() > 0) {
        deadlines_.cancel({detail::DeadlineKind::Idle, session_id});
    }
    detail::SessionSourceIds source_ids = session_manager_.sourceIds(session_id);
    std::optional<std::error_code> first_error;

    for (auto id : {source_ids.downstream, source_ids.upstream}) {
        if (poller_.isWatching(id)) {
            if (auto result = unregisterReactorSource(id); !result && !first_error) {
                first_error = result.error();
            }
        }
    }

    session_manager_.remove(session_id);
    if (shutdown_state_ == ShutdownState::GracefullyStopping && !hasActiveSessions()) {
        deadlines_.cancel(shutdown_deadline);
    }

    if (first_error) {
        return std::unexpected(first_error.value());
    }

    return {};
}

void ProxyReactor::closeSessionAndLog(detail::SessionId session_id) {
    if (auto result = closeSession(session_id); !result) {
        spdlog::error("Error closing session with ID {}: {}", session_id, result.error().message());
    }
}

bool ProxyReactor::hasActiveSessions() const { return !session_manager_.empty(); }

Result<detail::SourceId, std::error_code>
ProxyReactor::registerReactorSource(int fd, uint32_t initial_interests,
                                    const detail::ReactorRegistration& reactor_registration) {
    detail::SourceId source_id = sources_.add(reactor_registration);

    if (auto result = poller_.add(source_id, fd, initial_interests); !result) {
        sources_.remove(source_id);
        return std::unexpected(result.error());
    }

    return source_id;
}

// If everything succeeds then the endpoint is registered and the resulting state is consistent.
// If anything fails the object's state remains as if this function was not called.
Result<detail::SourceId, std::error_code>
ProxyReactor::registerEndpoint(int fd, uint32_t initial_interests, detail::SessionId session_id,
                               detail::EndpointRole role) {
    detail::EndpointRegistration registration = {
        .session_id = session_id,
        .role = role,
    };

    return registerReactorSource(fd, initial_interests, registration);
}

Status<std::error_code> ProxyReactor::unregisterReactorSource(detail::SourceId id) {
    auto result = poller_.retire(id);
    sources_.remove(id);
    return result;
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerListener(int fd) {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(fd, initial_events, detail::ListenerRegistration{});
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerShutdownSignalEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(shutdown_signal_fd_.get(), initial_events,
                                 detail::ShutdownSignalRegistration{});
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerTimerEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(timer_fd_.get(), initial_events, detail::TimerRegistration{});
}

Result<detail::SourceId, std::error_code>
ProxyReactor::registerPendingConnection(detail::PendingConnection pending_connection) {
    auto registered = registerReactorSource(pending_connection.attempted_connection_fd->get(),
                                            EPOLLOUT, detail::PendingDialRegistration{});
    if (registered) {
        deadlines_.schedule({detail::DeadlineKind::Connect, *registered},
                            pending_connection.deadline);
        pending_connections_.emplace(*registered, std::move(pending_connection));
    }
    return registered;
}

Result<detail::PendingConnection, std::error_code> ProxyReactor::takePending(detail::SourceId id) {
    auto it = pending_connections_.find(id);
    assert(it != pending_connections_.end());
    deadlines_.cancel({detail::DeadlineKind::Connect, id});
    auto retired = unregisterReactorSource(id);
    auto pending = std::move(it->second);
    pending_connections_.erase(it);
    if (!retired) {
        return std::unexpected(retired.error());
    }
    return pending;
}

Status<std::error_code> ProxyReactor::cancelPendingConnections() {
    Status<std::error_code> status;
    for (auto it = pending_connections_.begin(); it != pending_connections_.end();) {
        auto id = (it++)->first; // takePending erases only this entry.
        if (auto pending = takePending(id); !pending && status) {
            status = std::unexpected(pending.error());
        }
    }
    return status;
}

// If everything succeeds both endpoints are registered and SessionManager assumes ownership of
// their file descriptors. If registration fails, the reactor source changes are rolled back.
Result<detail::SessionId, AddSessionError> ProxyReactor::addSession(FileDescriptor upstream_fd,
                                                                    FileDescriptor downstream_fd,
                                                                    detail::Deadline deadline) {
    detail::SessionId session_id = session_manager_.allocateId();
    uint32_t initial_events = EPOLLIN | EPOLLRDHUP;

    auto downstream_register_result = registerEndpoint(
        downstream_fd.get(), initial_events, session_id, detail::EndpointRole::Downstream);
    if (!downstream_register_result) {
        return std::unexpected(AddSessionError{
            .message = downstream_register_result.error().message(),
            .status = RollbackStatus::Success,
        });
    }
    detail::SourceId downstream_id = downstream_register_result.value();

    auto upstream_register_result = registerEndpoint(upstream_fd.get(), initial_events, session_id,
                                                     detail::EndpointRole::Upstream);
    if (!upstream_register_result) {
        if (auto deregister_result = unregisterReactorSource(downstream_id); !deregister_result) {
            return std::unexpected(AddSessionError{
                .message = std::format(
                    "Upstream endpoint registration failed: {}; downstream rollback failed: {}",
                    upstream_register_result.error().message(),
                    deregister_result.error().message()),
                .status = RollbackStatus::Failure,
            });
        }

        return std::unexpected(AddSessionError{
            .message = upstream_register_result.error().message(),
            .status = RollbackStatus::Success,
        });
    }
    detail::SourceId upstream_id = upstream_register_result.value();

    // Check at the ownership handoff as registration itself consumes dial budget.
    if (time_source_.now() >= deadline) {
        auto upstream = unregisterReactorSource(upstream_id);
        auto downstream = unregisterReactorSource(downstream_id);
        if (!upstream || !downstream) {
            return std::unexpected(AddSessionError{
                .message = (!upstream ? upstream.error() : downstream.error()).message(),
                .status = RollbackStatus::Failure});
        }
        return std::unexpected(
            AddSessionError{"Connection establishment timed out", RollbackStatus::Success});
    }

    session_manager_.add(session_id, std::move(downstream_fd), std::move(upstream_fd),
                         detail::SessionSourceIds{
                             .downstream = downstream_id,
                             .upstream = upstream_id,
                         });

    if (timeouts_.idle.count() > 0) {
        deadlines_.schedule({detail::DeadlineKind::Idle, session_id},
                            time_source_.now() + timeouts_.idle);
    }
    return session_id;
}

Status<FatalReactorError> ProxyReactor::handleEndpoint(detail::EndpointRegistration registration,
                                                       uint32_t event_mask) {
    detail::SessionId session_id = registration.session_id;

    if (!session_manager_.contains(session_id)) {
        return {};
    }

    std::optional<detail::IdleDeadline> idle_deadline;
    if (timeouts_.idle.count() > 0) {
        auto at = deadlines_.scheduledAt({detail::DeadlineKind::Idle, session_id});
        idle_deadline.emplace(at.value(), timeouts_.idle, time_source_);
    }
    auto* idle = idle_deadline ? &*idle_deadline : nullptr;

    using SessionEventHandler = detail::SessionEventResult (detail::SessionManager::*)(
        detail::SessionId, detail::EndpointRole, detail::IdleDeadline*);

    // Error and full hangup supersede ordinary readiness in the same notification.
    // A full hangup has its own bounded read and cannot accept queued output.
    if (event_mask & (EPOLLERR | EPOLLHUP)) {
        auto result = (event_mask & EPOLLERR)
                          ? session_manager_.handleError(session_id, registration.role)
                          : session_manager_.handleHangup(session_id, registration.role, idle);
        if (auto applied = applySessionEventResult(session_id, result, idle); !applied) {
            return std::unexpected(applied.error());
        }
        return {};
    }

    static constexpr std::array<std::pair<uint32_t, SessionEventHandler>, 3> handlers = {{
        {EPOLLIN, &detail::SessionManager::handleReadable},
        {EPOLLOUT, &detail::SessionManager::handleWritable},
        {EPOLLRDHUP, &detail::SessionManager::handlePeerHalfClosed},
    }};

    for (const auto& [event_flag, handler] : handlers) {
        if (!(event_mask & event_flag)) {
            continue;
        }

        auto result = applySessionEventResult(
            session_id, (session_manager_.*handler)(session_id, registration.role, idle), idle);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed ||
            session_manager_.shouldRetire(session_id, registration.role)) {
            return {};
        }
    }

    return {};
}

Result<ProxyReactor::EndpointEventOutcome, FatalReactorError>
ProxyReactor::applySessionEventResult(detail::SessionId session_id,
                                      const detail::SessionEventResult& event_result,
                                      detail::IdleDeadline* idle) {
    if (event_result.error) {
        spdlog::error("Error in session {}: {}", session_id, *event_result.error);
    }

    if (event_result.action == detail::SessionEventAction::Close || (idle && idle->expired())) {
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    if (idle) {
        if (auto at = idle->takeRefresh()) {
            deadlines_.schedule({detail::DeadlineKind::Idle, session_id}, *at);
        }
    }
    if (auto result = synchronizeSessionInterests(session_id); !result) {
        return std::unexpected(FatalReactorError{result.error().message()});
    }

    return EndpointEventOutcome::KeepSession;
}

Status<std::error_code> ProxyReactor::synchronizeSessionInterests(detail::SessionId session_id) {
    detail::SessionSourceIds source_ids = session_manager_.sourceIds(session_id);
    detail::SessionInterests interests = session_manager_.interests(session_id);

    if (auto result = synchronizeEndpoint(session_id, detail::EndpointRole::Downstream,
                                          source_ids.downstream, interests.downstream);
        !result) {
        return result;
    }

    return synchronizeEndpoint(session_id, detail::EndpointRole::Upstream, source_ids.upstream,
                               interests.upstream);
}

Status<std::error_code>
ProxyReactor::synchronizeEndpoint(detail::SessionId session_id, detail::EndpointRole role,
                                  detail::SourceId source_id,
                                  const detail::EndpointInterests& interests) {
    // After residual EOF the source no longer needs even an edge-triggered watch.
    if (session_manager_.shouldRetire(session_id, role)) {
        if (!poller_.isWatching(source_id)) {
            return {};
        }
        return unregisterReactorSource(source_id);
    }

    return poller_.setInterests(source_id, getEventMask(interests));
}

Status<FatalReactorError> ProxyReactor::handleShutdownSignal(detail::ShutdownSignalRegistration) {
    auto drain_result = detail::readShutdownRequests(shutdown_signal_fd_.get());
    if (!drain_result) {
        return std::unexpected(FatalReactorError{drain_result.error().message()});
    }
    int count = drain_result.value();

    for (int i = 0; i < count; i++) {
        if (auto shutdown_request_result = handleShutdownRequest(); !shutdown_request_result) {
            return std::unexpected(FatalReactorError{shutdown_request_result.error().message()});
        }
    }

    return {};
}

Status<FatalReactorError> ProxyReactor::handleTimer(detail::TimerRegistration) {
    auto expired = detail::drainTimerFd(timer_fd_.get());
    if (!expired) {
        return std::unexpected(FatalReactorError{expired.error().message()});
    }
    if (*expired) {
        armed_deadline_.reset();
        if (auto result = expireDeadlines(time_source_.now()); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    }
    return {};
}

Status<std::error_code> ProxyReactor::synchronizeTimer() {
    // A simulated time point has no meaning to the kernel's CLOCK_MONOTONIC timer.
    if (!time_source_.usesSystemClock()) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
    auto next = deadlines_.next();
    if (next != armed_deadline_) {
        if (auto result = detail::setTimer(timer_fd_.get(), next); !result) {
            return result;
        }
        armed_deadline_ = next;
    }
    return {};
}

Status<std::error_code> ProxyReactor::expireDeadlines(detail::Deadline now) {
    // A burst of earlier idle expirations must not defer a due process shutdown.
    if (shutdown_state_ == ShutdownState::GracefullyStopping &&
        deadlines_.due(shutdown_deadline, now)) {
        return handleShutdownRequest();
    }
    for (int i = 0; i < max_expirations_per_dispatch; ++i) {
        auto key = deadlines_.popDue(now);
        if (!key) {
            break;
        }
        if (key->kind == detail::DeadlineKind::Idle) {
            closeSessionAndLog(key->id);
        } else if (key->kind == detail::DeadlineKind::Connect &&
                   pending_connections_.contains(key->id)) {
            auto pending = takePending(key->id);
            if (!pending) {
                return std::unexpected(pending.error());
            }
            spdlog::debug("Upstream connection establishment timed out");
        }
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::handleListener(detail::ListenerRegistration) {
    for (int i = 0; i < max_accept_batch_size; i++) {
        auto accept_result = active_listener_->listener.acceptClientConnection();
        if (!accept_result) {
            if (isRecoverableAcceptError(accept_result.error())) {
                spdlog::warn("Connection could not be accepted: {}",
                             accept_result.error().message());
                continue;
            }

            return std::unexpected(FatalReactorError{
                std::format("Listener accept failed: {}", accept_result.error().message())});
        }

        if (std::holds_alternative<net::AcceptWouldBlock>(*accept_result)) {
            return {};
        }

        auto accept_success = std::get<net::AcceptSuccess>(std::move(*accept_result));

        spdlog::info("Client {} connected", net::formatAddress(accept_success.remote));

        auto deadline = time_source_.now() + timeouts_.connect;
        auto dial_result = upstream_dialer_.dial(std::move(accept_success.fd),
                                                 accept_success.remote, deadline, time_source_);
        if (!dial_result) {
            if (auto error_result = handleDialError(std::move(dial_result.error()));
                !error_result) {
                return error_result;
            }

            continue;
        }

        if (auto dispatch_result = handleDialResult(std::move(*dial_result)); !dispatch_result) {
            return dispatch_result;
        }
    }

    return {};
}

Status<FatalReactorError> ProxyReactor::handlePendingConnection(detail::SourceId id) {
    auto pending = takePending(id);
    if (!pending) {
        return std::unexpected(FatalReactorError{pending.error().message()});
    }
    if (shutdown_state_ != ShutdownState::Running) {
        return {};
    }
    auto dial_result = upstream_dialer_.advanceDialAttempt(std::move(*pending), time_source_);
    if (!dial_result) {
        return handleDialError(std::move(dial_result.error()));
    }

    return handleDialResult(std::move(*dial_result));
}

Status<FatalReactorError> ProxyReactor::handleDialResult(detail::UpstreamDialResult dial_result) {
    if (auto* connected = std::get_if<detail::UpstreamDialConnected>(&dial_result)) {
        spdlog::info("Connected to {} on behalf of client {}",
                     net::formatAddress(connected->upstream_address),
                     net::formatAddress(connected->accepted_address));

        if (auto session_add_result =
                addSession(std::move(connected->upstream_connection_fd),
                           std::move(connected->accepted_connection_fd), connected->deadline);
            !session_add_result) {

            if (session_add_result.error().status == RollbackStatus::Failure) {
                return std::unexpected(
                    FatalReactorError{std::format("Reactor source registration rollback failed: {}",
                                                  session_add_result.error().message)});
            }

            spdlog::error("Failed to establish session between {} and {} : {}",
                          net::formatAddress(connected->accepted_address),
                          net::formatAddress(connected->upstream_address),
                          session_add_result.error().message);
            return {};
        }

        spdlog::info("Session between {} and {} established",
                     net::formatAddress(connected->accepted_address),
                     net::formatAddress(connected->upstream_address));

    } else if (auto* in_progress = std::get_if<detail::UpstreamDialInProgress>(&dial_result)) {
        if (time_source_.now() >= in_progress->pending_connection.deadline) {
            spdlog::debug("Upstream connection establishment timed out");
            return {};
        }
        net::SocketAddress accepted_address = in_progress->pending_connection.accepted_address;

        if (auto register_result =
                registerPendingConnection(std::move(in_progress->pending_connection));
            !register_result) {
            spdlog::error("Could not register upstream attempt: {}",
                          register_result.error().message());
            return {};
        }

        spdlog::debug("Pending connection for {} registered", net::formatAddress(accepted_address));

    } else {
        assert(false && "Invalid DialResult value");
    }

    return {};
}

Status<FatalReactorError> ProxyReactor::handleDialError(detail::UpstreamDialError dial_error) {
    const auto& upstream_endpoint = upstream_dialer_.upstreamEndpoint();
    spdlog::error("Connecting to {}:{} failed: {}", upstream_endpoint.hostname,
                  upstream_endpoint.port, dial_error.error_messages);

    if (dial_error.socket_creation_error) {
        return std::unexpected(FatalReactorError{
            std::format("Dial failed: {}", dial_error.socket_creation_error->message())});
    }

    return {};
}

Status<std::error_code> ProxyReactor::handleShutdownRequest() {
    switch (shutdown_state_) {
    case ShutdownState::Running:
        if (hasActiveSessions()) {
            deadlines_.schedule(shutdown_deadline, time_source_.now() + graceful_shutdown_timeout);
        }
        if (auto unregister_listener_result =
                unregisterReactorSource(active_listener_->listener_id);
            !unregister_listener_result) {
            return unregister_listener_result;
        }
        active_listener_.reset();
        shutdown_state_ = ShutdownState::GracefullyStopping;
        return cancelPendingConnections();
    case ShutdownState::GracefullyStopping:
        deadlines_.cancel(shutdown_deadline);
        shutdown_state_ = ShutdownState::HardStopping;
        performHardStop();
        break;
    case ShutdownState::HardStopping:
        // No-op
        break;
    }

    return {};
}

bool ProxyReactor::shouldStop() const {
    if (shutdown_state_ == ShutdownState::HardStopping) {
        return true;
    }

    if (shutdown_state_ == ShutdownState::GracefullyStopping && !hasActiveSessions()) {
        return true;
    }

    return false;
}

Status<std::error_code> ProxyReactor::forceCloseAllSessions() {
    std::optional<std::error_code> first_error;

    const auto ids = session_manager_.ids();
    for (auto it = ids.begin(); it != ids.end();) {
        const auto session_id = *it++; // Advance before closeSession erases the current owner.

        if (auto result = closeSession(session_id); !result) {
            spdlog::warn("Failed to close session {} during hard shutdown: {}", session_id,
                         result.error().message());

            if (!first_error) {
                first_error = result.error();
            }
        }
    }

    if (first_error) {
        return std::unexpected(first_error.value());
    }

    return {};
}

void ProxyReactor::performHardStop() {
    if (auto sessions_closure_result = forceCloseAllSessions(); !sessions_closure_result) {
        spdlog::error("Failed to close all sessions during hard shutdown: {}",
                      sessions_closure_result.error().message());
    }
}

} // namespace orbit::proxy
