// Known limitations of this implementation. These will be addressed gradually during development:
//
// 1. Single connection pair: This implementation handles exactly one downstream-upstream session.
// multi-connection support with per-pair state tracking must be added.
//
// 2. No idle timeout: If one peer half-closes and the other never responds with a FIN segment or
// sends any data to trigger socket error, the proxy sits idle forever. Idle-timeout teardown must
// be added to deal with this.
//
// 3. Error propagation is corase and one error stops the entire proxy. We need to add retries and
// some kind of graceful degradation, especially when multi-session support is added.

#include "proxy.h"

#include <cstdint>
#include <expected>
#include <iostream>
#include <system_error>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "../fd.h"
#include "epoll_utils.h"
#include "forwarding.h"
#include "pending_data_sender.h"
#include "send_buffer_factory.h"
#include "session_pair.h"

namespace orbit {

namespace {

#ifndef NDEBUG
inline void debug_epoll_event(const epoll_event& event) {
    uint32_t mask = event.events;
    SessionEndpoint* endpoint = reinterpret_cast<SessionEndpoint*>(event.data.ptr);
    std::cerr << "event fd=" << endpoint->socket_fd << " mask=0x" << std::hex << mask << std::dec
              << '\n';
}
#endif

std::expected<FileDescriptor, std::error_code> createEpollInstance() {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    return FileDescriptor(epfd);
}

std::expected<void, std::error_code> registerFileDescriptors(int epfd, SessionEndpoint& downstream,
                                                             SessionEndpoint& upstream) {
    epoll_event downstream_event = {
        .events = downstream.current_events,
        .data = {.ptr = &downstream},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_ADD, downstream.socket_fd, &downstream_event);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    epoll_event upstream_event = {
        .events = upstream.current_events,
        .data = {.ptr = &upstream},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_ADD, upstream.socket_fd, &upstream_event);
        result == -1) {
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
    return endpoint.other->peer_half_closed && endpoint.send_buffer->empty() &&
           !(endpoint.other->current_events & EPOLLIN);
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

std::expected<void, std::error_code> runEventLoop(int epfd) {
    constexpr size_t buf_cap = 4096;
    Forwarder forwarder(buf_cap, epfd);
    PendingDataSender sender(buf_cap, epfd);

    constexpr size_t event_buf_size = 64;
    epoll_event event_buf[event_buf_size];

    while (true) {
        int n = epoll_wait(epfd, event_buf, event_buf_size, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        for (int i = 0; i < n; i++) {
            uint32_t mask = event_buf[i].events;
            SessionEndpoint* endpoint = reinterpret_cast<SessionEndpoint*>(event_buf[i].data.ptr);

#ifndef NDEBUG
            debug_epoll_event(event_buf[i]);
#endif

            // There is at least one byte in the kernel receive buffer of the socket or EOF has been
            // reached. Registered by default since data arrives unpredictably and we always want to
            // try to forward it if possible.
            if (mask & EPOLLIN) {
                if (auto result = forwarder.forward(*endpoint); !result) {
                    return result;
                }

                // After forwarding data from this endpoint if we have reached EOF then the other
                // endpoint's outbound side may now be able to propagate a half-close.
                if (auto result = halfCloseIfReady(*endpoint->other); !result) {
                    return result;
                }

                if (shouldTearDown(*endpoint)) {
                    return {};
                }
            }

            // There is space in kernel send buffer, or at least send() syscall can accept some
            // bytes. Registered only when there is pending data in the user space send buffer and
            // unset otherwise.
            if (mask & EPOLLOUT) {
                if (auto result = sender.sendPending(*endpoint); !result) {
                    return result;
                }

                // After consuming data from the user space send buffer if it is empty then one of
                // the conditions for propagating a half-close through this endpoint was reached.
                if (auto result = halfCloseIfReady(*endpoint); !result) {
                    return result;
                }

                if (shouldTearDown(*endpoint)) {
                    return {};
                }
            }

            // Peer has closed their end of the TCP connection. After send buffer and kernel receive
            // buffer have been drained the half-close needs to propagate to the other peer in the
            // session. Given the use of level-triggered event distribution for epoll, the event is
            // unregistred after receiving it to prevent it from re-triggering on subsequent
            // epoll_wait() calls.
            if (mask & EPOLLRDHUP) {
                endpoint->peer_half_closed = true;
                if (auto result = modifyEpollEvents(*endpoint, endpoint->socket_fd, epfd,
                                                    endpoint->current_events & ~EPOLLRDHUP,
                                                    endpoint->current_events);
                    !result) {
                    return result;
                }

                // If it is already the case that the kernel receive buffer and user space send
                // buffer are empty we are able to immediately propagate the half close through end
                // other endpoint's outbound side.
                if (auto result = halfCloseIfReady(*endpoint->other); !result) {
                    return result;
                }

                if (shouldTearDown(*endpoint)) {
                    return {};
                }
            }

            // EPOLLHUP in the context of TCP sockets means that both directions of the connection
            // have been closed or the peer abruptly terminated the connection using RST segment.
            // EPOLLERR signals a socket-level error.
            // In both cases it's not possible to transmit any more data between the hosts in the
            // session so the data sitting in the buffer of the proxy cannot be delivered.
            if (mask & (EPOLLHUP | EPOLLERR)) {
                return getSocketErrorIfPresent(*endpoint);
            }
        }
    }
}

} // namespace

std::expected<void, std::error_code> runProxy(int downstream_fd, int upstream_fd) {
    auto epoll_create_result = createEpollInstance();
    if (!epoll_create_result) {
        return std::unexpected(epoll_create_result.error());
    }
    FileDescriptor epfd = std::move(epoll_create_result.value());

    constexpr size_t block_size = 4096;
    constexpr size_t high_watermark = block_size * 64;
    constexpr size_t low_watermark = block_size * 48;
    SendBufferFactory send_buffer_factory(Config{.block_size = block_size,
                                                 .high_watermark = high_watermark,
                                                 .low_watermark = low_watermark});
    std::unique_ptr<SessionPair> session =
        makeSessionPair(downstream_fd, upstream_fd, send_buffer_factory);

    uint32_t initial_events = EPOLLIN |   // fd has data to read or is at EOF
                              EPOLLRDHUP; // TCP FIN segment received
    session->downstream.current_events = initial_events;
    session->upstream.current_events = initial_events;

    if (auto register_result =
            registerFileDescriptors(epfd.get(), session->downstream, session->upstream);
        !register_result) {
        return register_result;
    }

    if (auto result = runEventLoop(epfd.get()); !result) {
        return result;
    }

    return {};
}

} // namespace orbit
