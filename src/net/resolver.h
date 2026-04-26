#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <sys/socket.h>

namespace orbit::net {

struct ResolvedAddress {
    socklen_t addrlen;     // length to pass to bind()/connect()
    sockaddr_storage addr; // address family, IP address, port number
};

struct ResolveError {
    std::string message;
};

std::expected<std::vector<ResolvedAddress>, ResolveError> resolve(const std::string& hostname,
                                                                  uint16_t port, bool passive);

} // namespace orbit::net
