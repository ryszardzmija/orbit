#include "net/dialer.h"

#include <cerrno>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include "common/fd.h"

namespace orbit::net {

std::expected<DialSuccess, DialError> dial(const SocketAddress& address) {
    while (true) {
        int fd = socket(address.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd == -1) {
            return std::unexpected(DialError{
                .code = std::error_code(errno, std::system_category()),
                .failed_op = DialFailedOp::Socket,
            });
        }
        FileDescriptor socket_fd(fd);

        int result = connect(fd, reinterpret_cast<const sockaddr*>(&address.addr), address.addrlen);

        if (result == -1 && errno == EINTR) {
            continue;
        }

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
}

} // namespace orbit::net
