// Known limitations of this implementation. These will be addressed gradually during development:
//
// 1. Blocking I/O: recv() and send() can both block in specific scenarios:
// - sendAll() blocks when the peer's receive buffer fills (closed TCP flow control window).
//   Under sustained asymmetric load this can block the event loop as thread waits until flow
//   control window opens. In the meantime proxy's receive buffer can accumulate and also cause the
//   flow control window to close on the other side which blocks the peer from sending more data to
//   the proxy.
// - recv() cannot be safely looped inside forwardAvailable() because the loop might block on empty
// buffer.
//   This forces one chunk per wake-up reads on the normal path. Inside drainReceiveBuffer() looping
//   is safe because received FIN segment guarantees eventual EOF.
//
// 2. Single connection pair: This implementation handles exactly one downstream-upstream session.
//    multi-connection support with per-pair state tracking must be added.
//
// 3. No idle timeout: If one peer half-closes and the other never responds with a FIN segment or
// sends any data
//    to trigger socket error, the proxy sits idle forever. Idle-timeout teardown must be added to
//    deal with this.
//
// 4. Error propagation is corase and one error stops the entire proxy. We need to add retries and
// some kind of
//    graceful degradation.

#include "proxy.h"

#include <cstdint>
#include <expected>
#include <iostream>
#include <system_error>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "fd.h"

namespace orbit {

namespace {

struct ConnectionPair {
    const int recv_fd;
    const int send_fd;
    uint32_t current_events;
};

#ifndef NDEBUG
inline void debug_epoll_event(const epoll_event& event) {
    uint32_t mask = event.events;
    ConnectionPair* conn_pair = reinterpret_cast<ConnectionPair*>(event.data.ptr);
    std::cerr << "event fd=" << conn_pair->recv_fd << " mask=0x" << std::hex << mask << std::dec
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

std::expected<void, std::error_code> registerFileDescriptors(int epfd, ConnectionPair& down_to_up,
                                                             ConnectionPair& up_to_down) {
    epoll_event downstream_reg = {
        .events = down_to_up.current_events,
        .data = {.ptr = &down_to_up},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_ADD, down_to_up.recv_fd, &downstream_reg);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    epoll_event upstream_reg = {
        .events = up_to_down.current_events,
        .data = {.ptr = &up_to_down},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_ADD, up_to_down.recv_fd, &upstream_reg);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> sendAll(int fd, const char* buf, size_t len) {
    size_t bytes_sent = 0;

    while (bytes_sent < len) {
        ssize_t n = send(fd, buf + bytes_sent, len - bytes_sent, MSG_NOSIGNAL);

        if (n == 0) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        if (n == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        bytes_sent += n;
    }

    return {};
}

enum class ReadResult {
    Eof,
    Success,
};

std::expected<ReadResult, std::error_code> forwardAvailable(int recv_fd, int send_fd) {
    constexpr size_t buf_size = 4096;
    char buf[buf_size];

    ssize_t bytes_read = recv(recv_fd, buf, buf_size, 0);

    if (bytes_read == 0) {
        return ReadResult::Eof;
    }

    if (bytes_read == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    // Drain the user-space buffer. This call could potentially block e.g., when the TCP flow
    // control window closes.
    if (auto result = sendAll(send_fd, buf, bytes_read); !result) {
        return std::unexpected(result.error());
    }

    return ReadResult::Success;
}

std::expected<void, std::error_code> drainReceiveBuffer(int recv_fd, int send_fd) {
    constexpr size_t buf_size = 4096;
    char buf[buf_size];

    // After EPOLLHUP event is received the connection is closed, so recv() will not block in this
    // loop but rather return 0 once all the data is read, which terminates the loop without
    // blocking.
    while (true) {
        ssize_t bytes_read = recv(recv_fd, buf, buf_size, 0);

        if (bytes_read == 0) {
            return {};
        }

        if (bytes_read == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        if (auto result = sendAll(send_fd, buf, bytes_read); !result) {
            return result;
        }
    }
}

std::expected<void, std::error_code> halfCloseConnection(int socket_fd) {
    int result = shutdown(socket_fd, SHUT_WR);
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> startForwarding(int epfd) {
    constexpr size_t event_buf_size = 64;
    epoll_event event_buf[event_buf_size];

    while (true) {
        int n = epoll_wait(epfd, event_buf, event_buf_size, -1);
        for (int i = 0; i < n; i++) {
            uint32_t mask = event_buf[i].events;
            ConnectionPair* conn_pair = reinterpret_cast<ConnectionPair*>(event_buf[i].data.ptr);

#ifndef NDEBUG
            debug_epoll_event(event_buf[i]);
#endif

            // Always try to read data first if available, even if other flags are set.
            if (mask & EPOLLIN) {
                auto result = forwardAvailable(conn_pair->recv_fd, conn_pair->send_fd);
                if (!result) {
                    return std::unexpected(result.error());
                }

                if (*result == ReadResult::Eof) {
                    conn_pair->current_events &= ~EPOLLIN;
                    epoll_event event_data = {
                        .events = conn_pair->current_events,
                        .data = event_buf[i].data,
                    };
                    if (int r = epoll_ctl(epfd, EPOLL_CTL_MOD, conn_pair->recv_fd, &event_data);
                        r == -1) {
                        return std::unexpected(std::error_code(errno, std::system_category()));
                    }
                }
            }

            // After peer's write side has closed this action needs to be propagated.
            if (mask & EPOLLRDHUP) {
                if (auto result = drainReceiveBuffer(conn_pair->recv_fd, conn_pair->send_fd);
                    !result) {
                    return result;
                }

                if (auto result = halfCloseConnection(conn_pair->send_fd); !result) {
                    return result;
                }

                conn_pair->current_events &= ~EPOLLRDHUP;

                epoll_event event_data = {
                    .events = conn_pair->current_events,
                    .data = event_buf[i].data,
                };
                if (int result = epoll_ctl(epfd, EPOLL_CTL_MOD, conn_pair->recv_fd, &event_data);
                    result == -1) {
                    return std::unexpected(std::error_code(errno, std::system_category()));
                }
            }

            // Peer has torn down the connection. Drain the receive buffer and close connection with
            // the other peer.
            if (mask & EPOLLHUP) {
                if (auto result = drainReceiveBuffer(conn_pair->recv_fd, conn_pair->send_fd);
                    !result) {
                    return result;
                }
                return {};
            }

            // Error detected at the socket layer. Since one of the connections is unusable we stop
            // forwarding.
            if (mask & EPOLLERR) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
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

    uint32_t initial_events = EPOLLIN |   // fd has data to read or is at EOF
                              EPOLLRDHUP; // TCP FIN segment received
    ConnectionPair down_to_up = {
        .recv_fd = downstream_fd,
        .send_fd = upstream_fd,
        .current_events = initial_events,
    };
    ConnectionPair up_to_down = {
        .recv_fd = upstream_fd,
        .send_fd = downstream_fd,
        .current_events = initial_events,
    };

    if (auto register_result = registerFileDescriptors(epfd.get(), down_to_up, up_to_down);
        !register_result) {
        return register_result;
    }

    if (auto result = startForwarding(epfd.get()); !result) {
        return result;
    }

    return {};
}

} // namespace orbit
