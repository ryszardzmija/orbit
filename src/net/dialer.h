#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "common/fd.h"
#include "net/socket_address.h"

namespace orbit::net {

struct DialError {
    std::string message;
};

struct DialSuccess {
    FileDescriptor fd;
    SocketAddress remote;
};

std::expected<DialSuccess, DialError> dial(const std::string& hostname, uint16_t port);

} // namespace orbit::net
