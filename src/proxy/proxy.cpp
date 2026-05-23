#include "proxy/proxy.h"

#include <cstdint>
#include <expected>
#include <memory>
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
#include "proxy/detail/epoll_utils.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/send_buffer_factory.h"
#include "proxy/detail/send_buffer_options.h"
#include "proxy/detail/session_pair.h"
#include "proxy/detail/signal_fd.h"

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

std::expected<void, std::error_code>
getSocketErrorIfPresent(const detail::SessionEndpoint& endpoint) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (auto result = getsockopt(endpoint.socket_fd, SOL_SOCKET, SO_ERROR, &err, &len);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    if (err != 0) {
        return std::unexpected(std::error_code(err, std::system_category()));
    }
    return {};
}

std::expected<void, std::error_code> drainSignalFd(int fd) {
    while (true) {
        signalfd_siginfo info = {};
        ssize_t bytes_read = read(fd, &info, sizeof(info));

        if (bytes_read == sizeof(info)) {
            continue;
        }

        if (bytes_read == -1 && errno == EAGAIN) {
            return {};
        }

        if (bytes_read == -1 && errno == EINTR) {
            continue;
        }

        if (bytes_read == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

} // namespace

ProxyReactor::ProxyReactor(FileDescriptor epfd, FileDescriptor shutdown_signal_fd)
    : epfd_(std::move(epfd)),
      shutdown_signal_fd_(std::move(shutdown_signal_fd)),
      send_buffer_factory_(detail::SendBufferOptions{.block_size = block_size,
                                                     .high_watermark = high_watermark,
                                                     .low_watermark = low_watermark}),
      forwarder_(forwarder_buf_cap, epfd_.get()),
      sender_(sender_buf_cap, epfd_.get()) {}

std::expected<ProxyReactor, std::error_code> ProxyReactor::create(FileDescriptor downstream_fd,
                                                                  FileDescriptor upstream_fd) {
    auto epoll_create_result = createEpollInstance();
    if (!epoll_create_result) {
        return std::unexpected(epoll_create_result.error());
    }
    FileDescriptor epfd = std::move(epoll_create_result.value());

    auto signalfd_create_result = detail::createShutdownSignalFd();
    if (!signalfd_create_result) {
        return std::unexpected(signalfd_create_result.error());
    }
    FileDescriptor signal_fd = std::move(signalfd_create_result.value());

    ProxyReactor reactor(std::move(epfd), std::move(signal_fd));

    if (auto register_result = reactor.registerShutdownSignalEvent(); !register_result) {
        return std::unexpected(register_result.error());
    }

    if (auto session_add_result =
            reactor.addSession(std::move(upstream_fd), std::move(downstream_fd));
        !session_add_result) {
        return std::unexpected(session_add_result.error());
    }

    return reactor;
}

std::expected<void, std::error_code> ProxyReactor::start() {
    spdlog::info("Starting proxying traffic...");

    epoll_event event_buf[event_buf_cap];

    while (true) {
        int n = epoll_wait(epfd_.get(), event_buf, event_buf_cap, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::error_code(errno, std::system_category()));
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

            // TODO: Decide what errors should be returned from the handlers and how to handle these
            // errors.
            if (auto* endpoint = std::get_if<detail::EndpointRegistration>(&registration)) {
                if (auto handler_result = handleEndpoint(*endpoint, event_mask); !handler_result) {
                    spdlog::error("Error in endpoint handler: {}",
                                  handler_result.error().message());
                    return handler_result;
                }
            } else if (auto* listener = std::get_if<detail::ListenerRegistration>(&registration)) {
                if (auto handler_result = handleListener(*listener); !handler_result) {
                    spdlog::error("Error in listener handler: {}",
                                  handler_result.error().message());
                    return handler_result;
                }
            } else if (auto* shutdown_signal =
                           std::get_if<detail::ShutdownSignalRegistration>(&registration)) {
                if (auto handler_result = handleShutdownSignal(*shutdown_signal); !handler_result) {
                    spdlog::error("Error in shutdown signal handler: {}",
                                  handler_result.error().message());
                    return handler_result;
                }
            }
        }

        // NOTE: This should be removed once event loop can accept new client connections.
        if (!hasActiveSessions()) {
            return {};
        }
    }
}

detail::SessionEndpoint& ProxyReactor::getEndpoint(detail::ManagedSession& managed_session,
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

detail::ReactorSourceId ProxyReactor::getEndpointId(detail::ManagedSession& managed_session,
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

detail::ReactorSourceId ProxyReactor::getOtherEndpointId(detail::ManagedSession& managed_session,
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

void ProxyReactor::closeSessionAndLog(detail::SessionId session_id) {
    if (auto result = closeSession(session_id); !result) {
        spdlog::error("Error closing session with ID {}: {}", session_id, result.error().message());
    }
}

bool ProxyReactor::hasActiveSessions() const { return !sessions_.empty(); }

std::expected<void, std::error_code>
ProxyReactor::registerReactorSource(int fd, uint32_t initial_events, detail::ReactorSourceId id,
                                    const detail::ReactorRegistration& reactor_registration) {
    if (auto result = registerFileDescriptor(epfd_.get(), fd, initial_events, id); !result) {
        return result;
    }

    auto [source_it, source_inserted] = registrations_.emplace(id, reactor_registration);
    assert(source_inserted);

    return {};
}

std::expected<void, std::error_code> ProxyReactor::registerEndpoint(int fd, uint32_t initial_events,
                                                                    detail::SessionId session_id,
                                                                    detail::EndpointRole role,
                                                                    detail::ReactorSourceId id) {
    detail::EndpointRegistration registration = {
        .session_id = session_id,
        .role = role,
    };

    return registerReactorSource(fd, initial_events, id, registration);
}

std::expected<void, std::error_code> ProxyReactor::registerShutdownSignalEvent() {
    uint32_t initial_events = EPOLLIN;
    detail::ReactorSourceId id = reactor_source_id_generator_.getNextId();

    return registerReactorSource(shutdown_signal_fd_.get(), initial_events, id,
                                 detail::ShutdownSignalRegistration{});
}

std::expected<void, std::error_code> ProxyReactor::addSession(FileDescriptor upstream_fd,
                                                              FileDescriptor downstream_fd) {
    detail::SessionId session_id = session_id_generator_.getNextId();
    uint32_t initial_events = EPOLLIN | EPOLLRDHUP;

    std::unique_ptr<detail::SessionPair> session_pair = makeSessionPair(
        downstream_fd.get(), upstream_fd.get(), initial_events, send_buffer_factory_);

    detail::ReactorSourceId downstream_id = reactor_source_id_generator_.getNextId();
    if (auto register_result = registerEndpoint(downstream_fd.get(), initial_events, session_id,
                                                detail::EndpointRole::Downstream, downstream_id);
        !register_result) {
        return std::unexpected(register_result.error());
    }

    detail::ReactorSourceId upstream_id = reactor_source_id_generator_.getNextId();
    if (auto register_result = registerEndpoint(upstream_fd.get(), initial_events, session_id,
                                                detail::EndpointRole::Upstream, upstream_id);
        !register_result) {
        return std::unexpected(register_result.error());
    }

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

// TODO: Think about whether errors for handling specific events should return error from handler
// or continue to try to process as many events as possible.
std::expected<void, std::error_code>
ProxyReactor::handleEndpoint(detail::EndpointRegistration registration, uint32_t event_mask) {
    detail::SessionId session_id = registration.session_id;

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return {};
    }

    detail::ManagedSession& managed_session = session_it->second;

    detail::SessionEndpoint& endpoint = getEndpoint(managed_session, registration.role);

    detail::EndpointContext context = {
        .endpoint = endpoint,
        .endpoint_id = getEndpointId(managed_session, registration.role),
        .other_endpoint_id = getOtherEndpointId(managed_session, registration.role)};

    // There is at least one byte in the kernel receive buffer of the socket or EOF has been
    // reached. Registered by default since data arrives unpredictably and we always want to
    // try to forward it if possible.
    if (event_mask & EPOLLIN) {
        if (auto result = forwarder_.forward(context); !result) {
            return result;
        }

        // After forwarding data from this endpoint if we have reached EOF then the other
        // endpoint's outbound side may now be able to propagate a half-close.
        if (auto result = halfCloseIfReady(*(endpoint.other)); !result) {
            return result;
        }

        if (shouldTearDown(endpoint)) {
            closeSessionAndLog(session_id);
            return {};
        }
    }

    // There is space in kernel send buffer, or at least send() syscall can accept some
    // bytes. Registered only when there is pending data in the user space send buffer and
    // unset otherwise.
    if (event_mask & EPOLLOUT) {
        if (auto result = sender_.sendPending(context); !result) {
            return result;
        }

        // After consuming data from the user space send buffer if it is empty then one of
        // the conditions for propagating a half-close through this endpoint was reached.
        if (auto result = halfCloseIfReady(endpoint); !result) {
            return result;
        }

        if (shouldTearDown(endpoint)) {
            closeSessionAndLog(session_id);
            return {};
        }
    }

    // Peer has closed their end of the TCP connection. After send buffer and kernel receive
    // buffer have been drained the half-close needs to propagate to the other peer in the
    // session. Given the use of level-triggered event distribution for epoll, the event is
    // unregistered after receiving it to prevent it from re-triggering on subsequent
    // epoll_wait() calls.
    if (event_mask & EPOLLRDHUP) {
        endpoint.peer_half_closed = true;
        if (auto result =
                modifyEpollEvents(context, endpoint.socket_fd, epfd_.get(),
                                  endpoint.current_events & ~EPOLLRDHUP, endpoint.current_events);
            !result) {
            return result;
        }

        // If it is already the case that the kernel receive buffer and user space send
        // buffer are empty we are able to immediately propagate the half close through the
        // other endpoint's outbound side.
        if (auto result = halfCloseIfReady(*(endpoint.other)); !result) {
            return result;
        }

        if (shouldTearDown(endpoint)) {
            closeSessionAndLog(session_id);
            return {};
        }
    }

    // EPOLLHUP in the context of TCP sockets means that both directions of the connection
    // have been closed or the peer abruptly terminated the connection using RST segment.
    // EPOLLERR signals a socket-level error.
    // In both cases it's not possible to transmit any more data between the hosts in the
    // session so the data sitting in the buffer of the proxy cannot be delivered.
    if (event_mask & (EPOLLHUP | EPOLLERR)) {
        closeSessionAndLog(session_id);
        return {};
    }

    return {};
}

// TODO: Implement the rest of the handlers.
std::expected<void, std::error_code>
ProxyReactor::handleShutdownSignal(detail::ShutdownSignalRegistration registration) {
    return drainSignalFd(shutdown_signal_fd_.get());
}

std::expected<void, std::error_code>
ProxyReactor::handleListener(detail::ListenerRegistration registration) {
    return {};
}

} // namespace orbit::proxy
