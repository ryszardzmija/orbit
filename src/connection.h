#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include "common/fd.h"

namespace orbit {

class Connection {
public:
    Connection(int socket_fd, const sockaddr_in& remote_address)
        : socket_(socket_fd),
          remote_address_(remote_address) {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    int socketFd() const { return socket_.get(); }

    in_addr_t address() const { return ntohl(remote_address_.sin_addr.s_addr); }

    in_port_t port() const { return ntohs(remote_address_.sin_port); }

private:
    FileDescriptor socket_;
    sockaddr_in remote_address_;
};

} // namespace orbit
