#pragma once

#include <cerrno>
#include <sys/socket.h>
#include <system_error>

#include "common/fd.h"

namespace orbit::proxy::test {

struct SocketPair {
    FileDescriptor local;
    FileDescriptor peer;
};

inline SocketPair socketPair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
        throw std::system_error(errno, std::system_category(), "socketpair");
    }
    return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

} // namespace orbit::proxy::test
