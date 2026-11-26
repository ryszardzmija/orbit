#include "net/dialer.h"

#include <cerrno>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include "common/fd.h"

namespace orbit::net {

DialFailureKind classifyDialError(DialError error) {
    const int code = error.code.value();
    switch (error.failed_op) {
    case DialFailedOp::Socket:
        switch (code) {
        case EMFILE:
        case ENFILE:
        case ENOMEM:
        case ENOBUFS:
            return DialFailureKind::Pressure;
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
        case EACCES:
        case EPERM:
            return DialFailureKind::Candidate;
        default:
            return DialFailureKind::Infrastructure;
        }
    case DialFailedOp::Connect:
    case DialFailedOp::Completion:
        return code == ENOMEM || code == ENOBUFS ? DialFailureKind::Pressure
                                                 : DialFailureKind::Candidate;
    case DialFailedOp::CompletionQuery:
        return DialFailureKind::Candidate;
    case DialFailedOp::Deadline:
        return DialFailureKind::Timeout;
    }
    return DialFailureKind::Infrastructure;
}

const char* dialOperationName(DialFailedOp operation) {
    switch (operation) {
    case DialFailedOp::Socket:
        return "upstream socket";
    case DialFailedOp::Connect:
        return "connect";
    case DialFailedOp::Completion:
        return "connect completion";
    case DialFailedOp::CompletionQuery:
        return "getsockopt(SO_ERROR)";
    case DialFailedOp::Deadline:
        return "connection establishment deadline";
    }
    return "upstream dial";
}

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
