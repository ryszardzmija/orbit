#pragma once

#include <sys/socket.h>

namespace orbit::net {

struct SocketAddress {
    socklen_t addrlen;     // length to pass to bind()/connect()
    sockaddr_storage addr; // address family, IP address, port number
};

} // namespace orbit::net
