#pragma once

#include <sys/socket.h>

namespace orbit::net {

// Owns a POSIX socket address together with the number of valid bytes in `addr`.
// `addrlen` must be passed with `addr` to socket APIs such as bind(), connect(),
// getsockname(), and getnameinfo().
struct SocketAddress {
    socklen_t addrlen;
    sockaddr_storage addr;
};

} // namespace orbit::net
