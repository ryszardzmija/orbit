#include "net/dialer.h"

#include <cerrno>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include "common/fd.h"

namespace orbit::net {

std::expected<DialSuccess, DialError> dial(const SocketAddress& address,
                                           std::chrono::steady_clock::time_point deadline) {
    int fd = socket(address.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return std::unexpected(DialError{
            .code = std::error_code(errno, std::system_category()),
            .failed_op = DialFailedOp::Socket,
        });
    }
    FileDescriptor socket_fd(fd);

    // Socket creation can consume the remaining budget for this attempt.
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(DialError{
            .code = std::make_error_code(std::errc::timed_out),
            .failed_op = DialFailedOp::Deadline,
        });
    }
    int result = connect(fd, reinterpret_cast<const sockaddr*>(&address.addr), address.addrlen);

    if (result == -1 && errno == EINPROGRESS) {
        return DialSuccess{
            .fd = std::move(socket_fd),
            .state = ConnectState::InProgress,
        };
    }

    if (result == -1) {
        return std::unexpected(DialError{
            .code = std::error_code(errno, std::system_category()),
            .failed_op = DialFailedOp::Connect,
        });
    }

    return DialSuccess{
        .fd = std::move(socket_fd),
        .state = ConnectState::Connected,
    };
}

} // namespace orbit::net
