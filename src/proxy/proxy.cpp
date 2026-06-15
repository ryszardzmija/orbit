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

bool isFatalDialError(const std::error_code& error) {
    switch (error.value()) {
    case EADDRINUSE:
    case EAGAIN:
    case EALREADY:
    case ECONNREFUSED:
    case EISCONN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ETIMEDOUT:
        return false;
    default:
        return true;
    }
}

std::optional<std::error_code> getFirstFatalError(const std::vector<std::error_code>& codes) {
    for (const auto& code : codes) {
        if (isFatalDialError(code)) {
            return code;
        }
    }

    return std::nullopt;
}

uint32_t getEventMask(const detail::EndpointInterests& interests) {
    uint32_t event_mask = 0;
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
                           FileDescriptor shutdown_timer_fd, detail::UpstreamDialer upstream_dialer)
    : poller_(std::move(poller)),
      session_manager_(detail::SessionManagerOptions{
          .send_buffer =
              {
                  .block_size = block_size,
                  .high_watermark = high_watermark,
                  .low_watermark = low_watermark,
              },
          .forwarder_buffer_capacity = forwarder_buf_cap,
          .sender_buffer_capacity = sender_buf_cap,
      }),
      shutdown_signal_fd_(std::move(shutdown_signal_fd)),
      shutdown_timer_fd_(std::move(shutdown_timer_fd)),
      upstream_dialer_(std::move(upstream_dialer)) {}

Result<ProxyReactor, ProxyCreateError>
ProxyReactor::create(const net::ListenSocketAddress& listen_address,
                     const net::ResolutionEndpoint& upstream_address) {
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

    auto timerfd_create_result = detail::createShutdownTimerFd();
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
                         std::move(dialer));

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

    if (auto register_result = reactor.registerShutdownTimerEvent(); !register_result) {
        return std::unexpected(ProxyCreateError{register_result.error().message()});
    }

    return std::move(reactor);
}

Status<ProxyRuntimeError> ProxyReactor::start() {
    spdlog::info("Starting proxying traffic...");

    while (!shouldStop()) {
        auto wait_result = poller_.wait();
        if (!wait_result) {
            return std::unexpected(ProxyRuntimeError{wait_result.error().message()});
        }

        for (const detail::ReadyEvent& event : *wait_result) {
            auto registration = sources_.find(event.source_id);

            if (!registration) {
                continue;
            }

            auto handler_result = std::visit(
                overloaded{
                    [&](detail::EndpointRegistration r) { return handleEndpoint(r, event.events); },
                    [&](detail::ListenerRegistration r) { return handleListener(r); },
                    [&](detail::ShutdownSignalRegistration r) { return handleShutdownSignal(r); },
                    [&](detail::ShutdownTimerRegistration r) { return handleShutdownTimer(r); },
                    [&](detail::PendingDialRegistration r) {
                        return handlePendingConnection(event.source_id, r);
                    },
                },
                *registration);

            if (!handler_result) {
                spdlog::error("Error in reactor event handler: {}", handler_result.error().message);
                return std::unexpected(ProxyRuntimeError{handler_result.error().message});
            }

            if (shouldStop()) {
                break;
            }
        }
    }

    return {};
}

Status<std::error_code> ProxyReactor::closeSession(detail::SessionId session_id) {
    if (!session_manager_.contains(session_id)) {
        return {};
    }

    detail::SessionSourceIds source_ids = session_manager_.sourceIds(session_id);
    std::optional<std::error_code> first_error;

    // An endpoint that hung up is retired during interest synchronization, so only retire the
    // endpoints that are still watched to avoid deregistering them twice.
    if (poller_.isWatching(source_ids.downstream)) {
        if (auto result = poller_.retire(source_ids.downstream); !result) {
            first_error = result.error();
        }
        sources_.remove(source_ids.downstream);
    }

    if (poller_.isWatching(source_ids.upstream)) {
        if (auto result = poller_.retire(source_ids.upstream); !result) {
            if (!first_error) {
                first_error = result.error();
            }
        }
        sources_.remove(source_ids.upstream);
    }

    session_manager_.remove(session_id);

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
    if (auto result = poller_.remove(id); !result) {
        return result;
    }

    sources_.remove(id);
    return {};
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

Result<detail::SourceId, std::error_code> ProxyReactor::registerShutdownTimerEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(shutdown_timer_fd_.get(), initial_events,
                                 detail::ShutdownTimerRegistration{});
}

Result<detail::SourceId, std::error_code>
ProxyReactor::registerPendingConnection(detail::PendingConnection pending_connection) {
    uint32_t initial_events = EPOLLOUT;
    int fd = pending_connection.attempted_connection_fd->get();
    detail::PendingConnectionId id = pending_connection_id_generator_.getNextId();

    pending_connections_.emplace(id, std::move(pending_connection));

    auto register_result =
        registerReactorSource(fd, initial_events, detail::PendingDialRegistration{id});
    if (!register_result) {
        pending_connections_.erase(id);
    }
    return register_result;
}

// If everything succeeds both endpoints are registered and SessionManager assumes ownership of
// their file descriptors. If registration fails, the reactor source changes are rolled back.
Status<AddSessionError> ProxyReactor::addSession(FileDescriptor upstream_fd,
                                                 FileDescriptor downstream_fd) {
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

    session_manager_.add(session_id, std::move(downstream_fd), std::move(upstream_fd),
                         detail::SessionSourceIds{
                             .downstream = downstream_id,
                             .upstream = upstream_id,
                         });

    return {};
}

Status<FatalReactorError> ProxyReactor::handleEndpoint(detail::EndpointRegistration registration,
                                                       uint32_t event_mask) {
    detail::SessionId session_id = registration.session_id;

    if (!session_manager_.contains(session_id)) {
        return {};
    }

    using SessionEventHandler = detail::SessionEventResult (detail::SessionManager::*)(
        detail::SessionId, detail::EndpointRole);

    // Each ready epoll event is dispatched to the matching SessionManager handler, in priority
    // order. EPOLLIN runs first so an endpoint is always drained before any teardown decision,
    // and EPOLLERR runs before EPOLLHUP so an abrupt failure tears the session down rather than
    // attempting a graceful hangup drain.
    static constexpr std::array<std::pair<uint32_t, SessionEventHandler>, 5> handlers = {{
        // There is at least one byte in the kernel receive buffer of the socket or EOF has been
        // reached. Registered by default since data arrives unpredictably and we always want to
        // try to forward it if possible.
        {EPOLLIN, &detail::SessionManager::handleReadable},
        // There is space in the kernel send buffer, or send() can accept some bytes. This is
        // registered only while the endpoint has pending outbound data.
        {EPOLLOUT, &detail::SessionManager::handleWritable},
        // The peer closed its outbound side. SessionManager records the state and propagates the
        // half-close after all buffered inbound data has been forwarded.
        {EPOLLRDHUP, &detail::SessionManager::handlePeerHalfClosed},
        // A socket-level error occurred. The session cannot continue and is torn down.
        {EPOLLERR, &detail::SessionManager::handleError},
        // The connection hung up. SessionManager preserves any remaining inbound data, stops
        // writing to the socket, and closes the session once the surviving direction has drained.
        {EPOLLHUP, &detail::SessionManager::handleHangup},
    }};

    for (const auto& [event_flag, handler] : handlers) {
        if (!(event_mask & event_flag)) {
            continue;
        }

        auto result = applySessionEventResult(
            session_id, (session_manager_.*handler)(session_id, registration.role));
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    return {};
}

Result<ProxyReactor::EndpointEventOutcome, FatalReactorError>
ProxyReactor::applySessionEventResult(detail::SessionId session_id,
                                      const detail::SessionEventResult& event_result) {
    if (event_result.error) {
        spdlog::error("Error in session {}: {}", session_id, *event_result.error);
    }

    if (event_result.action == detail::SessionEventAction::Close) {
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
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
    // A hung-up endpoint can produce no further useful events, but EPOLLHUP is reported
    // unconditionally, so leaving it registered would spin the reactor. Retire it from the poller
    // while keeping the session alive so the surviving direction can finish flushing. Retirement
    // is permanent, so a source already retired by an earlier sync is simply skipped.
    if (session_manager_.shouldRetire(session_id, role)) {
        if (!poller_.isWatching(source_id)) {
            return {};
        }
        if (auto result = poller_.retire(source_id); !result) {
            return result;
        }
        sources_.remove(source_id);
        return {};
    }

    return poller_.setInterests(source_id, getEventMask(interests));
}

Status<FatalReactorError> ProxyReactor::handleShutdownSignal(detail::ShutdownSignalRegistration) {
    auto drain_result = detail::drainSignalFd(shutdown_signal_fd_.get());
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

Status<FatalReactorError> ProxyReactor::handleShutdownTimer(detail::ShutdownTimerRegistration) {
    if (auto drain_result = detail::drainTimerFd(shutdown_timer_fd_.get()); !drain_result) {
        return std::unexpected(FatalReactorError{drain_result.error().message()});
    }

    if (auto shutdown_request_result = handleShutdownRequest(); !shutdown_request_result) {
        return std::unexpected(FatalReactorError{shutdown_request_result.error().message()});
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

        auto dial_result =
            upstream_dialer_.dial(std::move(accept_success.fd), accept_success.remote);
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

Status<FatalReactorError>
ProxyReactor::handlePendingConnection(detail::SourceId id,
                                      detail::PendingDialRegistration registration) {
    auto it = pending_connections_.find(registration.pending_connection_id);
    assert(it != pending_connections_.end());

    detail::PendingConnection pending_connection = std::move(it->second);
    pending_connections_.erase(registration.pending_connection_id);

    if (auto unregister_result = unregisterReactorSource(id); !unregister_result) {
        return std::unexpected(FatalReactorError{unregister_result.error().message()});
    }

    auto dial_result = upstream_dialer_.advanceDialAttempt(std::move(pending_connection));
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

        if (auto session_add_result = addSession(std::move(connected->upstream_connection_fd),
                                                 std::move(connected->accepted_connection_fd));
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
        net::SocketAddress accepted_address = in_progress->pending_connection.accepted_address;

        if (auto register_result =
                registerPendingConnection(std::move(in_progress->pending_connection));
            !register_result) {
            return std::unexpected(FatalReactorError{register_result.error().message()});
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

    std::optional<std::error_code> fatal_error = getFirstFatalError(dial_error.error_codes);
    if (fatal_error) {
        return std::unexpected(
            FatalReactorError{std::format("Dial failed: {}", fatal_error->message())});
    }

    return {};
}

Status<std::error_code> ProxyReactor::handleShutdownRequest() {
    switch (shutdown_state_) {
    case ShutdownState::Running:
        if (auto timer_arming_result =
                detail::armTimer(shutdown_timer_fd_.get(), graceful_shutdown_timeout_s);
            !timer_arming_result) {
            return timer_arming_result;
        }
        if (auto unregister_listener_result =
                unregisterReactorSource(active_listener_->listener_id);
            !unregister_listener_result) {
            return unregister_listener_result;
        }
        active_listener_.reset();
        shutdown_state_ = ShutdownState::GracefullyStopping;
        break;
    case ShutdownState::GracefullyStopping:
        if (auto timer_disarming_result = detail::disarmTimer(shutdown_timer_fd_.get());
            !timer_disarming_result) {
            return timer_disarming_result;
        }
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

    while (!session_manager_.empty()) {
        detail::SessionId session_id = session_manager_.firstId();

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
