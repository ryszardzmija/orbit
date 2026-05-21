#include "proxy/proxy.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "common/fd.h"
#include "proxy/epoll_utils.h"
#include "proxy/forwarding.h"
#include "proxy/pending_data_sender.h"
#include "proxy/send_buffer_factory.h"
#include "proxy/send_buffer_options.h"
#include "proxy/session_pair.h"

namespace orbit {

namespace {

std::expected<FileDescriptor, std::error_code> createEpollInstance() {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    return FileDescriptor(epfd);
}

std::expected<void, std::error_code>
registerFileDescriptor(int epfd, int fd, uint32_t initial_events, EndpointId endpoint_id) {
    epoll_event events = {
        .events = initial_events,
        .data = {.u64 = endpoint_id},
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

bool shouldHalfClose(const SessionEndpoint& endpoint) {
    return endpoint.other->peer_half_closed && endpoint.other->done_reading &&
           endpoint.send_buffer->empty();
}

std::expected<void, std::error_code> halfCloseIfReady(SessionEndpoint& endpoint) {
    if (shouldHalfClose(endpoint) && !endpoint.half_close_sent) {
        if (auto result = halfCloseConnection(endpoint.socket_fd); !result) {
            return result;
        }
        endpoint.half_close_sent = true;
    }

    return {};
}

// Whether the session associated with this endpoint should be torn down.
bool shouldTearDown(const SessionEndpoint& endpoint) {
    return endpoint.half_close_sent && endpoint.other->half_close_sent;
}

std::expected<void, std::error_code> getSocketErrorIfPresent(const SessionEndpoint& endpoint) {
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

} // namespace

ProxyReactor::ProxyReactor(
    FileDescriptor epfd, std::unordered_map<SessionId, ManagedSession> sessions,
    std::unordered_map<EndpointId, EndpointRegistration> endpoint_registrations,
    SessionIdGenerator session_id_generator, EndpointIdGenerator endpoint_id_generator)
    : epfd_(std::move(epfd)),
      sessions_(std::move(sessions)),
      endpoint_registrations_(std::move(endpoint_registrations)),
      session_id_generator_(session_id_generator),
      endpoint_id_generator_(endpoint_id_generator) {}

std::expected<ProxyReactor, std::error_code> ProxyReactor::create(FileDescriptor downstream_fd,
                                                                  FileDescriptor upstream_fd) {
    auto epoll_create_result = createEpollInstance();
    if (!epoll_create_result) {
        return std::unexpected(epoll_create_result.error());
    }
    FileDescriptor epfd = std::move(epoll_create_result.value());

    SendBufferFactory send_buffer_factory(SendBufferOptions{.block_size = block_size,
                                                            .high_watermark = high_watermark,
                                                            .low_watermark = low_watermark});

    std::unique_ptr<SessionPair> session_pair =
        makeSessionPair(downstream_fd.get(), upstream_fd.get(), send_buffer_factory);

    std::unordered_map<SessionId, ManagedSession> sessions;
    std::unordered_map<EndpointId, EndpointRegistration> endpoint_registrations;

    SessionIdGenerator session_id_generator;
    EndpointIdGenerator endpoint_id_generator;

    SessionId session_id = session_id_generator.getNextId();
    EndpointId downstream_id = endpoint_id_generator.getNextId();
    EndpointId upstream_id = endpoint_id_generator.getNextId();

    uint32_t initial_events = EPOLLIN | EPOLLRDHUP;
    session_pair->downstream.current_events = initial_events;
    session_pair->upstream.current_events = initial_events;

    if (auto register_result =
            registerFileDescriptor(epfd.get(), downstream_fd.get(), initial_events, downstream_id);
        !register_result) {
        return std::unexpected(register_result.error());
    }
    if (auto register_result =
            registerFileDescriptor(epfd.get(), upstream_fd.get(), initial_events, upstream_id);
        !register_result) {
        return std::unexpected(register_result.error());
    }

    ManagedSession managed_session{
        .session =
            {
                .downstream_fd = std::move(downstream_fd),
                .upstream_fd = std::move(upstream_fd),
                .endpoints = std::move(session_pair),
            },
        .downstream_endpoint_id = downstream_id,
        .upstream_endpoint_id = upstream_id,
    };
    auto [it, inserted] = sessions.emplace(session_id, std::move(managed_session));
    assert(inserted);

    EndpointRegistration downstream_registration{
        .session_id = session_id,
        .role = EndpointRole::Downstream,
    };
    auto [downstream_it, downstream_inserted] =
        endpoint_registrations.emplace(downstream_id, downstream_registration);
    assert(downstream_inserted);

    EndpointRegistration upstream_registration{
        .session_id = session_id,
        .role = EndpointRole::Upstream,
    };
    auto [upstream_it, upstream_inserted] =
        endpoint_registrations.emplace(upstream_id, upstream_registration);
    assert(upstream_inserted);

    return ProxyReactor(std::move(epfd), std::move(sessions), std::move(endpoint_registrations),
                        session_id_generator, endpoint_id_generator);
}

std::expected<void, std::error_code> ProxyReactor::start() {
    Forwarder forwarder(forwarder_buf_cap, epfd_.get());
    PendingDataSender sender(forwarder_buf_cap, epfd_.get());

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
            uint32_t mask = event_buf[i].events;
            EndpointId endpoint_id = static_cast<EndpointId>(event_buf[i].data.u64);

            auto it = endpoint_registrations_.find(endpoint_id);
            if (it == endpoint_registrations_.end()) {
                continue;
            }
            EndpointRegistration& registration = it->second;
            SessionId session_id = registration.session_id;

            auto session_it = sessions_.find(session_id);
            if (session_it == sessions_.end()) {
                continue;
            }
            ManagedSession& managed_session = session_it->second;

            SessionEndpoint& endpoint = getEndpoint(managed_session, registration.role);

            EndpointContext context = {.endpoint = endpoint,
                                       .endpoint_id = endpoint_id,
                                       .other_endpoint_id =
                                           getOtherEndpointId(managed_session, registration.role)};

            spdlog::debug("epoll event fd={} mask={:#x}", endpoint.socket_fd, mask);

            // There is at least one byte in the kernel receive buffer of the socket or EOF has been
            // reached. Registered by default since data arrives unpredictably and we always want to
            // try to forward it if possible.
            if (mask & EPOLLIN) {
                if (auto result = forwarder.forward(context); !result) {
                    return result;
                }

                // After forwarding data from this endpoint if we have reached EOF then the other
                // endpoint's outbound side may now be able to propagate a half-close.
                if (auto result = halfCloseIfReady(*(endpoint.other)); !result) {
                    return result;
                }

                if (shouldTearDown(endpoint)) {
                    closeSessionAndLog(session_id);
                    continue;
                }
            }

            // There is space in kernel send buffer, or at least send() syscall can accept some
            // bytes. Registered only when there is pending data in the user space send buffer and
            // unset otherwise.
            if (mask & EPOLLOUT) {
                if (auto result = sender.sendPending(context); !result) {
                    return result;
                }

                // After consuming data from the user space send buffer if it is empty then one of
                // the conditions for propagating a half-close through this endpoint was reached.
                if (auto result = halfCloseIfReady(endpoint); !result) {
                    return result;
                }

                if (shouldTearDown(endpoint)) {
                    closeSessionAndLog(session_id);
                    continue;
                }
            }

            // Peer has closed their end of the TCP connection. After send buffer and kernel receive
            // buffer have been drained the half-close needs to propagate to the other peer in the
            // session. Given the use of level-triggered event distribution for epoll, the event is
            // unregistered after receiving it to prevent it from re-triggering on subsequent
            // epoll_wait() calls.
            if (mask & EPOLLRDHUP) {
                endpoint.peer_half_closed = true;
                if (auto result = modifyEpollEvents(context, endpoint.socket_fd, epfd_.get(),
                                                    endpoint.current_events & ~EPOLLRDHUP,
                                                    endpoint.current_events);
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
                    continue;
                }
            }

            // EPOLLHUP in the context of TCP sockets means that both directions of the connection
            // have been closed or the peer abruptly terminated the connection using RST segment.
            // EPOLLERR signals a socket-level error.
            // In both cases it's not possible to transmit any more data between the hosts in the
            // session so the data sitting in the buffer of the proxy cannot be delivered.
            if (mask & (EPOLLHUP | EPOLLERR)) {
                closeSessionAndLog(session_id);
                continue;
            }
        }

        // NOTE: This should be removed once event loop can accept new client connections.
        if (!hasActiveSessions()) {
            return {};
        }
    }
}

SessionEndpoint& ProxyReactor::getEndpoint(ManagedSession& managed_session, EndpointRole role) {
    switch (role) {
    case EndpointRole::Upstream:
        return managed_session.session.endpoints->upstream;
    case EndpointRole::Downstream:
        return managed_session.session.endpoints->downstream;
    }

    assert(false && "Invalid EndpointRole value");
    std::unreachable();
}

EndpointId ProxyReactor::getOtherEndpointId(ManagedSession& managed_session, EndpointRole role) {
    switch (role) {
    case EndpointRole::Upstream:
        return managed_session.downstream_endpoint_id;
    case EndpointRole::Downstream:
        return managed_session.upstream_endpoint_id;
    }

    assert(false && "Invalid EndpointRole value");
    std::unreachable();
}

std::expected<void, std::error_code> ProxyReactor::closeSession(SessionId session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {};
    }

    ManagedSession& managed_session = it->second;

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

    endpoint_registrations_.erase(managed_session.downstream_endpoint_id);
    endpoint_registrations_.erase(managed_session.upstream_endpoint_id);

    sessions_.erase(it);

    return {};
}

void ProxyReactor::closeSessionAndLog(SessionId session_id) {
    if (auto result = closeSession(session_id); !result) {
        spdlog::error("Error closing session with ID {}: {}", session_id, result.error().message());
    }
}

bool ProxyReactor::hasActiveSessions() const { return !sessions_.empty(); }

} // namespace orbit
