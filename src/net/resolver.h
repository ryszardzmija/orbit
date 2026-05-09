#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <sys/socket.h>

#include "net/socket_address.h"

namespace orbit::net {

struct ResolveError {
    std::string message;
};

std::expected<std::vector<SocketAddress>, ResolveError> resolve(const std::string& hostname,
                                                                uint16_t port, bool passive);

} // namespace orbit::net
