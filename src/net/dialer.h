#pragma once

#include <chrono>
#include <expected>
#include <system_error>

#include "common/fd.h"
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
    Deadline,
};

struct DialError {
    std::error_code code;
    DialFailedOp failed_op;
};

std::expected<DialSuccess, DialError> dial(const SocketAddress& address,
                                           std::chrono::steady_clock::time_point deadline);

} // namespace orbit::net
