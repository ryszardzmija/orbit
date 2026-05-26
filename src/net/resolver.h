#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <sys/socket.h>

#include "net/socket_address.h"

namespace orbit::net {

struct ResolutionEndpoint {
    std::string hostname;
    uint16_t port;
};

struct ResolveError {
    std::string message;
};

// Resolves `endpoint` into TCP socket address. `passive` parameter controls
// whether the address is suitable for binding to a passive socket or using with connect().
//
// This function is synchronous and may block while resolving a hostname.
// Returns an error if name resolution fails or no usable addresses are found.
std::expected<std::vector<SocketAddress>, ResolveError> resolve(const ResolutionEndpoint& endpoint,
                                                                bool passive);

} // namespace orbit::net
