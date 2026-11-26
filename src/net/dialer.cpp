#include "net/dialer.h"

#include <cerrno>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include "common/fd.h"

namespace orbit::net {

std::expected<DialSuccess, DialError> dial(const SocketAddress& address,
                                           SteadyTimeSource::TimePoint deadline,
                                           const SteadyTimeSource& time_source,
                                           const DialOperations& operations) {
    const int family = address.addr.ss_family;
    constexpr int socket_type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
    int fd = operations.socket ? operations.socket(operations.context, family, socket_type, 0)
                               : ::socket(family, socket_type, 0);
    if (fd == -1) {
        return std::unexpected(DialError{
            .code = std::error_code(errno, std::system_category()),
            .failed_op = DialFailedOp::Socket,
        });
    }
    FileDescriptor socket_fd(fd);

    // Socket creation can consume the remaining budget for this attempt.
    if (time_source.now() >= deadline) {
        return std::unexpected(DialError{
            .code = std::make_error_code(std::errc::timed_out),
            .failed_op = DialFailedOp::Deadline,
        });
    }
    const auto* socket_address = reinterpret_cast<const sockaddr*>(&address.addr);
    int result = operations.connect
                     ? operations.connect(operations.context, fd, socket_address, address.addrlen)
                     : ::connect(fd, socket_address, address.addrlen);

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
