#include "proxy/proxy.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

#include <cerrno>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/fd.h"
#include "net/address_format.h"
#include "net/socket_options.h"
#include "proxy/detail/epoll_utils.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/send_buffer_factory.h"
#include "proxy/detail/send_buffer_options.h"
#include "proxy/detail/session_pair.h"
#include "proxy/detail/signal_fd.h"
#include "proxy/detail/timer_fd.h"

namespace orbit::proxy {

namespace {

std::expected<FileDescriptor, std::error_code> createEpollInstance() {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    return FileDescriptor(epfd);
}

std::expected<void, std::error_code>
registerFileDescriptor(int epfd, int fd, uint32_t initial_events, uint64_t payload) {
    epoll_event events = {
        .events = initial_events,
        .data = {.u64 = payload},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &events); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> deregisterFileDescriptor(int epfd, int fd) {
    if (int result = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> halfCloseConnection(int socket_fd) {
    int result = shutdown(socket_fd, SHUT_WR);
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

bool shouldHalfClose(const detail::SessionEndpoint& endpoint) {
    return endpoint.other->peer_half_closed && endpoint.other->done_reading &&
           endpoint.send_buffer->empty();
}

std::expected<void, std::error_code> halfCloseIfReady(detail::SessionEndpoint& endpoint) {
    if (shouldHalfClose(endpoint) && !endpoint.half_close_sent) {
        if (auto result = halfCloseConnection(endpoint.socket_fd); !result) {
            return result;
        }
        endpoint.half_close_sent = true;
    }

    return {};
}

// Whether the session associated with this endpoint should be torn down.
bool shouldTearDown(const detail::SessionEndpoint& endpoint) {
    return endpoint.half_close_sent && endpoint.other->half_close_sent;
}

detail::SessionEndpoint& getEndpoint(detail::ManagedSession& managed_session,
                                     detail::EndpointRole role) {
    switch (role) {
    case detail::EndpointRole::Upstream:
        return managed_session.session.endpoints->upstream;
    case detail::EndpointRole::Downstream:
        return managed_session.session.endpoints->downstream;
    }

    assert(false && "Invalid EndpointRole value");
    std::unreachable();
}

detail::ReactorSourceId getEndpointId(detail::ManagedSession& managed_session,
                                      detail::EndpointRole role) {
    switch (role) {
    case detail::EndpointRole::Upstream:
        return managed_session.upstream_endpoint_id;
    case detail::EndpointRole::Downstream:
        return managed_session.downstream_endpoint_id;
    }

    assert(false && "Invalid EndpointRole value");
    std::unreachable();
}

detail::ReactorSourceId getOtherEndpointId(detail::ManagedSession& managed_session,
                                           detail::EndpointRole role) {
    switch (role) {
    case detail::EndpointRole::Upstream:
        return managed_session.downstream_endpoint_id;
    case detail::EndpointRole::Downstream:
        return managed_session.upstream_endpoint_id;
    }

    assert(false && "Invalid EndpointRole value");
    std::unreachable();
}

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

} // namespace

ProxyReactor::ProxyReactor(FileDescriptor epfd, FileDescriptor shutdown_signal_fd,
                           FileDescriptor shutdown_timer_fd, detail::Forwarder forwarder,
                           detail::PendingDataSender sender,
                           const net::DialSocketAddress& dial_address)
    : epfd_(std::move(epfd)),
      shutdown_signal_fd_(std::move(shutdown_signal_fd)),
      shutdown_timer_fd_(std::move(shutdown_timer_fd)),
      send_buffer_factory_(detail::SendBufferOptions{.block_size = block_size,
                                                     .high_watermark = high_watermark,
                                                     .low_watermark = low_watermark}),
      forwarder_(std::move(forwarder)),
      sender_(std::move(sender)),
      dial_address(dial_address) {}

std::expected<ProxyReactor, ProxyCreateError>
ProxyReactor::create(const net::ListenSocketAddress& listen_address,
                     const net::DialSocketAddress& dial_address) {
    auto epoll_create_result = createEpollInstance();
    if (!epoll_create_result) {
        return std::unexpected(ProxyCreateError{epoll_create_result.error().message()});
    }
    FileDescriptor epfd = std::move(epoll_create_result.value());

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

    detail::Forwarder forwarder = detail::Forwarder::create(forwarder_buf_cap);

    detail::PendingDataSender sender = detail::PendingDataSender::create(sender_buf_cap);

    auto listener_create_result = net::Listener::create(listen_address, max_backlog_size);
    if (!listener_create_result) {
        return std::unexpected(ProxyCreateError{listener_create_result.error().message});
    }
    net::Listener listener = std::move(listener_create_result.value());

    ProxyReactor reactor(std::move(epfd), std::move(signal_fd), std::move(timer_fd),
                         std::move(forwarder), std::move(sender), dial_address);

    auto listener_register_result = reactor.registerListener(listener.fd());
    if (!listener_register_result) {
        return std::unexpected(ProxyCreateError{listener_register_result.error().message()});
    }
    detail::ReactorSourceId listener_id = listener_register_result.value();

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

    return reactor;
}

std::expected<void, ProxyRuntimeError> ProxyReactor::start() {
    spdlog::info("Starting proxying traffic...");

    epoll_event event_buf[event_buf_cap];

    while (!shouldStop()) {
        int n = epoll_wait(epfd_.get(), event_buf, event_buf_cap, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(ProxyRuntimeError{std::system_category().message(errno)});
        }

        for (int i = 0; i < n; i++) {
            uint32_t event_mask = event_buf[i].events;
            detail::ReactorSourceId source_id =
                static_cast<detail::ReactorSourceId>(event_buf[i].data.u64);

            auto it = registrations_.find(source_id);
            if (it == registrations_.end()) {
                continue;
            }

            detail::ReactorRegistration& registration = it->second;

            if (auto* endpoint = std::get_if<detail::EndpointRegistration>(&registration)) {
                if (auto handler_result = handleEndpoint(*endpoint, event_mask); !handler_result) {
                    spdlog::error("Error in endpoint handler: {}", handler_result.error().message);
                    return std::unexpected(ProxyRuntimeError{handler_result.error().message});
                }
            } else if (auto* listener = std::get_if<detail::ListenerRegistration>(&registration)) {
                if (auto handler_result = handleListener(*listener); !handler_result) {
                    spdlog::error("Error in listener handler: {}", handler_result.error().message);
                    return std::unexpected(ProxyRuntimeError{handler_result.error().message});
                }
            } else if (auto* shutdown_signal =
                           std::get_if<detail::ShutdownSignalRegistration>(&registration)) {
                if (auto handler_result = handleShutdownSignal(*shutdown_signal); !handler_result) {
                    spdlog::error("Error in shutdown signal handler: {}",
                                  handler_result.error().message);
                    return std::unexpected(ProxyRuntimeError{handler_result.error().message});
                }
            } else if (auto* shutdown_timer =
                           std::get_if<detail::ShutdownTimerRegistration>(&registration)) {
                if (auto handler_result = handleShutdownTimer(*shutdown_timer); !handler_result) {
                    spdlog::error("Error in shutdown timer handler: {}",
                                  handler_result.error().message);
                    return std::unexpected(ProxyRuntimeError{handler_result.error().message});
                }
            } else {
                assert(false && "Invalid ReactorRegistration dispatch");
            }

            if (shouldStop()) {
                break;
            }
        }
    }

    return {};
}

std::expected<void, std::error_code> ProxyReactor::closeSession(detail::SessionId session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {};
    }

    detail::ManagedSession& managed_session = it->second;

    if (auto result =
            deregisterFileDescriptor(epfd_.get(), managed_session.session.downstream_fd.get());
        !result) {
        return result;
    }
    if (auto result =
            deregisterFileDescriptor(epfd_.get(), managed_session.session.upstream_fd.get());
        !result) {
        return result;
    }

    registrations_.erase(managed_session.downstream_endpoint_id);
    registrations_.erase(managed_session.upstream_endpoint_id);

    sessions_.erase(it);

    return {};
}

std::expected<void, std::error_code> ProxyReactor::forceCloseSession(detail::SessionId session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {};
    }

    detail::ManagedSession& managed_session = it->second;

    std::optional<std::error_code> first_error;

    if (auto result =
            deregisterFileDescriptor(epfd_.get(), managed_session.session.downstream_fd.get());
        !result) {
        spdlog::warn("Failed to deregister downstream socket for session {} from epoll: {}",
                     session_id, result.error().message());
        first_error = result.error();
    }
    if (auto result =
            deregisterFileDescriptor(epfd_.get(), managed_session.session.upstream_fd.get());
        !result) {
        spdlog::warn("Failed to deregister upstream socket for session {} from epoll: {}",
                     session_id, result.error().message());
        if (!first_error) {
            first_error = result.error();
        }
    }

    registrations_.erase(managed_session.downstream_endpoint_id);
    registrations_.erase(managed_session.upstream_endpoint_id);

    sessions_.erase(it);

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

bool ProxyReactor::hasActiveSessions() const { return !sessions_.empty(); }

std::expected<detail::ReactorSourceId, std::error_code>
ProxyReactor::registerReactorSource(int fd, uint32_t initial_events,
                                    const detail::ReactorRegistration& reactor_registration) {
    detail::ReactorSourceId id = reactor_source_id_generator_.getNextId();

    if (auto result = registerFileDescriptor(epfd_.get(), fd, initial_events, id); !result) {
        return std::unexpected(result.error());
    }

    auto [source_it, source_inserted] = registrations_.emplace(id, reactor_registration);
    assert(source_inserted);

    return id;
}

// If everything succeeds then the endpoint is registered and the resulting state is consistent.
// If anything fails the object's state remains as if this function was not called.
std::expected<detail::ReactorSourceId, std::error_code>
ProxyReactor::registerEndpoint(int fd, uint32_t initial_events, detail::SessionId session_id,
                               detail::EndpointRole role) {
    detail::EndpointRegistration registration = {
        .session_id = session_id,
        .role = role,
    };

    return registerReactorSource(fd, initial_events, registration);
}

std::expected<void, std::error_code>
ProxyReactor::unregisterReactorSource(int fd, detail::ReactorSourceId id) {
    auto result = deregisterFileDescriptor(epfd_.get(), fd);
    registrations_.erase(id);

    if (!result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<detail::ReactorSourceId, std::error_code> ProxyReactor::registerListener(int fd) {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(fd, initial_events, detail::ListenerRegistration{});
}

std::expected<detail::ReactorSourceId, std::error_code>
ProxyReactor::registerShutdownSignalEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(shutdown_signal_fd_.get(), initial_events,
                                 detail::ShutdownSignalRegistration{});
}

std::expected<detail::ReactorSourceId, std::error_code> ProxyReactor::registerShutdownTimerEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(shutdown_timer_fd_.get(), initial_events,
                                 detail::ShutdownTimerRegistration{});
}

// If everything succeeds both endpoints are registered, maps are populated, and file descriptors
// are owned by ProxyReactor.
// If any operation fails, all changes are rolled back. No endpoint from the attemped session
// remains registered or stored.
std::expected<void, AddSessionError> ProxyReactor::addSession(FileDescriptor upstream_fd,
                                                              FileDescriptor downstream_fd) {
    detail::SessionId session_id = session_id_generator_.getNextId();
    uint32_t initial_events = EPOLLIN | EPOLLRDHUP;

    std::unique_ptr<detail::SessionPair> session_pair = makeSessionPair(
        downstream_fd.get(), upstream_fd.get(), initial_events, send_buffer_factory_);

    auto downstream_register_result = registerEndpoint(
        downstream_fd.get(), initial_events, session_id, detail::EndpointRole::Downstream);
    if (!downstream_register_result) {

        return std::unexpected(AddSessionError{
            .message = downstream_register_result.error().message(),
            .status = RollbackStatus::Success,
        });
    }
    detail::ReactorSourceId downstream_id = downstream_register_result.value();

    auto upstream_register_result = registerEndpoint(upstream_fd.get(), initial_events, session_id,
                                                     detail::EndpointRole::Upstream);
    if (!upstream_register_result) {
        if (auto deregister_result = unregisterReactorSource(downstream_fd.get(), downstream_id);
            !deregister_result) {
            return std::unexpected(AddSessionError{
                .message = upstream_register_result.error().message(),
                .status = RollbackStatus::Failure,
            });
        }

        return std::unexpected(AddSessionError{
            .message = upstream_register_result.error().message(),
            .status = RollbackStatus::Success,
        });
    }
    detail::ReactorSourceId upstream_id = upstream_register_result.value();

    detail::ManagedSession managed_session{
        .session =
            {
                .downstream_fd = std::move(downstream_fd),
                .upstream_fd = std::move(upstream_fd),
                .endpoints = std::move(session_pair),
            },
        .downstream_endpoint_id = downstream_id,
        .upstream_endpoint_id = upstream_id,
    };

    auto [session_it, session_inserted] = sessions_.emplace(session_id, std::move(managed_session));
    assert(session_inserted);

    return {};
}

std::expected<void, FatalReactorError>
ProxyReactor::handleEndpoint(detail::EndpointRegistration registration, uint32_t event_mask) {
    detail::SessionId session_id = registration.session_id;

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return {};
    }

    detail::ManagedSession& managed_session = session_it->second;

    detail::SessionEndpoint& endpoint = getEndpoint(managed_session, registration.role);

    detail::ReactorSourceId endpoint_id = getEndpointId(managed_session, registration.role);
    detail::ReactorSourceId other_endpoint_id =
        getOtherEndpointId(managed_session, registration.role);

    // There is at least one byte in the kernel receive buffer of the socket or EOF has been
    // reached. Registered by default since data arrives unpredictably and we always want to
    // try to forward it if possible.
    if (event_mask & EPOLLIN) {
        auto result = handleEndpointReadable(session_id, endpoint_id, endpoint, other_endpoint_id,
                                             *(endpoint.other));
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    // There is space in kernel send buffer, or at least send() syscall can accept some
    // bytes. Registered only when there is pending data in the user space send buffer and
    // unset otherwise.
    if (event_mask & EPOLLOUT) {
        auto result = handleEndpointWritable(session_id, other_endpoint_id, *(endpoint.other),
                                             endpoint_id, endpoint);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    // Peer has closed their end of the TCP connection. After send buffer and kernel receive
    // buffer have been drained the half-close needs to propagate to the other peer in the
    // session. Given the use of level-triggered event distribution for epoll, the event is
    // unregistered after receiving it to prevent it from re-triggering on subsequent
    // epoll_wait() calls.
    if (event_mask & EPOLLRDHUP) {
        auto result = handleEndpointPeerHalfClosed(session_id, endpoint_id, endpoint);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    // EPOLLHUP in the context of TCP sockets means that both directions of the connection
    // have been closed or the peer abruptly terminated the connection using RST segment.
    // It's not possible to transmit any more data between the hosts in the session, so
    // data sitting the proxy's buffers cannot be delivered.
    if (event_mask & EPOLLHUP) {
        auto result = handleEndpointHangup(session_id);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    // EPOLLERR signals a socket-level error.
    // In this case session cannot continue and must be torn down.
    if (event_mask & EPOLLERR) {
        auto result = handleEndpointError(session_id, endpoint);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed) {
            return {};
        }
    }

    return {};
}

std::expected<EndpointEventOutcome, FatalReactorError> ProxyReactor::handleEndpointReadable(
    detail::SessionId session_id, detail::ReactorSourceId source_id,
    detail::SessionEndpoint& source, detail::ReactorSourceId destination_id,
    detail::SessionEndpoint& destination) {

    auto forward_result = forwarder_.forward(source);
    if (!forward_result) {
        spdlog::error("Forwarding failed in session {}: {}", session_id,
                      forward_result.error().message);
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    // Synchronize epoll interest list state
    detail::ForwardResult state_to_synchronize = forward_result.value();

    if (state_to_synchronize.source_reading_allowed) {
        if (auto result = ensureReadableInterest(source_id, source); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    } else {
        if (auto result = ensureNoReadableInterest(source_id, source); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    }

    if (state_to_synchronize.destination_has_pending_data) {
        if (auto result = ensureWritableInterest(destination_id, destination); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    } else {
        if (auto result = ensureNoWritableInterest(destination_id, destination); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    }

    // After forwarding data from this endpoint if we have reached EOF then the other
    // endpoint's outbound side may now be able to propagate a half-close.
    if (auto result = halfCloseIfReady(destination); !result) {
        spdlog::error("Half-closing of connection failed in session {}: {}", session_id,
                      result.error().message());
        return EndpointEventOutcome::KeepSession;
    }

    // TODO: Figure out if closing session fails should it be retried? Maybe
    // more granular approach is necessary to prevent reactor state from silently
    // becoming inconsistent?
    if (shouldTearDown(source)) {
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    return EndpointEventOutcome::KeepSession;
}

std::expected<EndpointEventOutcome, FatalReactorError> ProxyReactor::handleEndpointWritable(
    detail::SessionId session_id, detail::ReactorSourceId source_id,
    detail::SessionEndpoint& source, detail::ReactorSourceId destination_id,
    detail::SessionEndpoint& destination) {
    auto send_result = sender_.sendPending(destination);
    if (!send_result) {
        spdlog::error("Pending outbound data sending failed in session {}: {}", session_id,
                      send_result.error().message);
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    // Synchronize epoll interest list state
    detail::SendResult state_to_synchronize = send_result.value();

    if (state_to_synchronize.source_reading_allowed) {
        if (auto result = ensureReadableInterest(source_id, source); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    } else {
        if (auto result = ensureNoReadableInterest(source_id, source); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    }

    if (state_to_synchronize.destination_buffer_drained) {
        if (auto result = ensureNoWritableInterest(destination_id, destination); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    } else {
        if (auto result = ensureWritableInterest(destination_id, destination); !result) {
            return std::unexpected(FatalReactorError{result.error().message()});
        }
    }

    // After consuming data from the user space send buffer if it is empty then one of
    // the conditions for propagating a half-close through this endpoint was reached.
    if (auto result = halfCloseIfReady(destination); !result) {
        spdlog::error("Half-closing of connection failed in session {}: {}", session_id,
                      result.error().message());
        return EndpointEventOutcome::KeepSession;
    }

    if (shouldTearDown(destination)) {
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    return EndpointEventOutcome::KeepSession;
}

std::expected<EndpointEventOutcome, FatalReactorError>
ProxyReactor::handleEndpointPeerHalfClosed(detail::SessionId session_id,
                                           detail::ReactorSourceId endpoint_id,
                                           detail::SessionEndpoint& endpoint) {
    endpoint.peer_half_closed = true;

    if (auto result = disablePeerHalfCloseEvents(endpoint_id, endpoint); !result) {
        return std::unexpected(FatalReactorError{result.error().message()});
    }

    // If it is already the case that the kernel receive buffer and user space send
    // buffer are empty we are able to immediately propagate the half close through the
    // other endpoint's outbound side.
    if (auto result = halfCloseIfReady(*(endpoint.other)); !result) {
        spdlog::error("Half-closing of connection failed in session {}: {}", session_id,
                      result.error().message());
        return EndpointEventOutcome::KeepSession;
    }

    if (shouldTearDown(endpoint)) {
        closeSessionAndLog(session_id);
        return EndpointEventOutcome::SessionClosed;
    }

    return EndpointEventOutcome::KeepSession;
}

std::expected<EndpointEventOutcome, FatalReactorError>
ProxyReactor::handleEndpointHangup(detail::SessionId session_id) {
    closeSessionAndLog(session_id);
    return EndpointEventOutcome::SessionClosed;
}

std::expected<EndpointEventOutcome, FatalReactorError>
ProxyReactor::handleEndpointError(detail::SessionId session_id, detail::SessionEndpoint& endpoint) {
    int socket_error = 0;
    socklen_t len = sizeof(socket_error);

    if (int result = getsockopt(endpoint.socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &len);
        result == -1) {
        spdlog::error("Failed to read socket error for session {}: {}", session_id,
                      std::system_category().message(errno));
    } else {
        spdlog::error("Socket error in session {}: {}", session_id,
                      std::system_category().message(socket_error));
    }

    closeSessionAndLog(session_id);
    return EndpointEventOutcome::SessionClosed;
}

std::expected<void, std::error_code>
ProxyReactor::ensureReadableInterest(detail::ReactorSourceId source_id,
                                     detail::SessionEndpoint& endpoint) {
    if (endpoint.current_events & EPOLLIN) {
        return {};
    }

    if (auto result =
            detail::modifyEpollEvents(source_id, endpoint, endpoint.socket_fd, epfd_.get(),
                                      endpoint.current_events | EPOLLIN, endpoint.current_events);
        !result) {
        return result;
    }

    return {};
}

std::expected<void, std::error_code>
ProxyReactor::ensureNoReadableInterest(detail::ReactorSourceId source_id,
                                       detail::SessionEndpoint& endpoint) {
    if (!(endpoint.current_events & EPOLLIN)) {
        return {};
    }

    if (auto result =
            detail::modifyEpollEvents(source_id, endpoint, endpoint.socket_fd, epfd_.get(),
                                      endpoint.current_events & ~EPOLLIN, endpoint.current_events);
        !result) {
        return result;
    }

    return {};
}

std::expected<void, std::error_code>
ProxyReactor::ensureWritableInterest(detail::ReactorSourceId source_id,
                                     detail::SessionEndpoint& endpoint) {
    if (endpoint.current_events & EPOLLOUT) {
        return {};
    }

    if (auto result =
            detail::modifyEpollEvents(source_id, endpoint, endpoint.socket_fd, epfd_.get(),
                                      endpoint.current_events | EPOLLOUT, endpoint.current_events);
        !result) {
        return result;
    }

    return {};
}

std::expected<void, std::error_code>
ProxyReactor::ensureNoWritableInterest(detail::ReactorSourceId source_id,
                                       detail::SessionEndpoint& endpoint) {
    if (!(endpoint.current_events & EPOLLOUT)) {
        return {};
    }

    if (auto result =
            detail::modifyEpollEvents(source_id, endpoint, endpoint.socket_fd, epfd_.get(),
                                      endpoint.current_events & ~EPOLLOUT, endpoint.current_events);
        !result) {
        return result;
    }

    return {};
}

std::expected<void, std::error_code>
ProxyReactor::disablePeerHalfCloseEvents(detail::ReactorSourceId source_id,
                                         detail::SessionEndpoint& endpoint) {
    if (!(endpoint.current_events & EPOLLRDHUP)) {
        return {};
    }

    if (auto result = detail::modifyEpollEvents(source_id, endpoint, endpoint.socket_fd,
                                                epfd_.get(), endpoint.current_events & ~EPOLLRDHUP,
                                                endpoint.current_events);
        !result) {
        return result;
    }

    return {};
}

std::expected<void, FatalReactorError>
ProxyReactor::handleShutdownSignal(detail::ShutdownSignalRegistration) {
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

std::expected<void, FatalReactorError>
ProxyReactor::handleShutdownTimer(detail::ShutdownTimerRegistration) {
    if (auto drain_result = detail::drainTimerFd(shutdown_timer_fd_.get()); !drain_result) {
        return std::unexpected(FatalReactorError{drain_result.error().message()});
    }

    if (auto shutdown_request_result = handleShutdownRequest(); !shutdown_request_result) {
        return std::unexpected(FatalReactorError{shutdown_request_result.error().message()});
    }

    return {};
}

std::expected<void, FatalReactorError> ProxyReactor::handleListener(detail::ListenerRegistration) {
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

        auto dial_result = net::dial(dial_address);
        if (!dial_result) {
            spdlog::error("Error connecting to {}:{} : {}", dial_address.hostname,
                          dial_address.port, dial_result.error().message);
            // FileDescriptor ensures accepted socket is closed.
            continue;
        }

        auto dial_success = std::move(dial_result.value());

        spdlog::info("Connected to {} on behalf of client {}",
                     net::formatAddress(dial_success.remote),
                     net::formatAddress(accept_success.remote));

        // NOTE: Right now socket created by net::dial() is blocking to make connect() call blocking
        // so this should be removed after this is no longer the case.
        if (auto result = net::setNonBlocking(dial_success.fd.get()); !result) {
            spdlog::error("Error making upstream socket non-blocking: {}",
                          result.error().message());
            continue;
        }

        if (auto session_add_result =
                addSession(std::move(dial_success.fd), std::move(accept_success.fd));
            !session_add_result) {

            if (session_add_result.error().status == RollbackStatus::Failure) {
                return std::unexpected(
                    FatalReactorError{std::format("Reactor source registration rollback failed: {}",
                                                  session_add_result.error().message)});
            }

            spdlog::error("Failed to establish session between {} and {} : {}",
                          net::formatAddress(accept_success.remote),
                          net::formatAddress(dial_success.remote),
                          session_add_result.error().message);
            continue;
        }

        spdlog::info("Session between {} and {} established",
                     net::formatAddress(accept_success.remote),
                     net::formatAddress(dial_success.remote));
    }

    return {};
}

std::expected<void, std::error_code> ProxyReactor::handleShutdownRequest() {
    switch (shutdown_state_) {
    case ShutdownState::Running:
        if (auto timer_arming_result =
                detail::armTimer(shutdown_timer_fd_.get(), graceful_shutdown_timeout_s);
            !timer_arming_result) {
            return timer_arming_result;
        }
        if (auto unregister_listener_result = unregisterReactorSource(
                active_listener_->listener.fd(), active_listener_->listener_id);
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

std::expected<void, std::error_code> ProxyReactor::forceCloseAllSessions() {
    std::optional<std::error_code> first_error;

    while (!sessions_.empty()) {
        detail::SessionId session_id = sessions_.begin()->first;

        if (auto result = forceCloseSession(session_id); !result) {
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
