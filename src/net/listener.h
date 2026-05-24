#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <variant>

#include "common/fd.h"
#include "net/socket_address.h"

namespace orbit::net {

struct AcceptSuccess {
    FileDescriptor fd;
    SocketAddress remote;
};

struct AcceptWouldBlock {};

using AcceptResult = std::variant<AcceptSuccess, AcceptWouldBlock>;

struct ListenError {
    std::string message;
};

struct ListenSocketAddress {
    std::string interface;
    uint16_t port;
};

class Listener {
public:
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Listener(Listener&&) = default;
    Listener& operator=(Listener&&) = default;

    int fd() const { return listen_fd_.get(); }
    const SocketAddress& localAddress() const { return local_address_; }
    std::expected<AcceptResult, std::error_code> acceptClientConnection();

    static std::expected<Listener, ListenError> create(const ListenSocketAddress& address,
                                                       int max_backlog_size);

private:
    explicit Listener(FileDescriptor fd, SocketAddress local_address);

    FileDescriptor listen_fd_;
    SocketAddress local_address_;
};

} // namespace orbit::net
