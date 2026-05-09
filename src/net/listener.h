#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "common/fd.h"
#include "net/socket_address.h"

namespace orbit::net {

struct AcceptError {
    std::string message;
};

struct AcceptSuccess {
    FileDescriptor fd;
    SocketAddress remote;
};

struct ListenError {
    std::string message;
};

class Listener {
public:
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Listener(Listener&&) = default;
    Listener& operator=(Listener&&) = default;

    int fd() const { return listen_fd_.get(); }
    const SocketAddress& localAddress() const { return local_address_; }
    std::expected<AcceptSuccess, AcceptError> acceptClientConnection();

    static std::expected<Listener, ListenError> create(const std::string& interface, uint16_t port);

private:
    explicit Listener(FileDescriptor fd, SocketAddress local_address);

    FileDescriptor listen_fd_;
    SocketAddress local_address_;
};

} // namespace orbit::net
