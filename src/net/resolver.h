#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <sys/socket.h>

namespace orbit::net {

struct ResolvedAddress {
    int family;            // AF_INET or AF_INET6
    socklen_t addrlen;     // length to pass to bind()/connect()
    sockaddr_storage addr; // the address itself
};

struct ResolveError {
    std::string message;
};

std::expected<std::vector<ResolvedAddress>, ResolveError> resolve(const std::string& hostname,
                                                                  uint16_t port, bool passive);

} // namespace orbit::net
