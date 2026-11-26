#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"
#include "common/steady_time_source.h"
#include "net/socket_address.h"

namespace orbit::net {

enum class ConnectState {
    Connected,
    InProgress,
};

struct DialSuccess {
    FileDescriptor fd;
    ConnectState state;
};

enum class DialFailedOp {
    Socket,
    Connect,
    Completion,
    CompletionQuery,
    Deadline,
};

struct DialError {
    std::error_code code;
    DialFailedOp failed_op;
};

enum class DialFailureKind { Candidate, Timeout, Pressure, Infrastructure };

DialFailureKind classifyDialError(DialError error);
const char* dialOperationName(DialFailedOp operation);

// Optional syscall overrides borrow their context and follow the syscall errno conventions.
struct DialOperations {
    void* context = nullptr;
    int (*socket)(void*, int domain, int type, int protocol) = nullptr;
    int (*connect)(void*, int fd, const sockaddr* address, socklen_t length) = nullptr;
    int (*getsockopt)(void*, int fd, int level, int option, void* value,
                      socklen_t* length) = nullptr;
};

std::expected<DialSuccess, DialError> dial(const SocketAddress& address,
                                           SteadyTimeSource::TimePoint deadline,
                                           const SteadyTimeSource& time_source,
                                           const DialOperations& operations);

} // namespace orbit::net
