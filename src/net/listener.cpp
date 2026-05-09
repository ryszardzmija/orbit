#include "listener.h"

#include <cerrno>
#include <expected>
#include <format>
#include <system_error>
#include <utility>

#include <sys/socket.h>

#include "common/fd.h"
#include "net/address_format.h"
#include "net/resolver.h"
#include "net/socket_options.h"
#include "socket_address.h"

namespace orbit::net {

namespace {

constexpr int max_backlog_size = 10;

std::expected<FileDescriptor, std::error_code> createTcpSocket(const SocketAddress& address) {
    int fd = socket(address.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

std::expected<void, std::error_code> bindAddress(int socket_fd, const SocketAddress& address) {
    int result = bind(socket_fd, reinterpret_cast<const sockaddr*>(&address.addr), address.addrlen);
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> enterListenState(int socket_fd) {
    int result = listen(socket_fd, max_backlog_size);
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<SocketAddress, std::error_code> getSocketLocalAddress(int socket_fd) {
    sockaddr_storage addr = {};
    socklen_t addrlen = sizeof(addr);

    if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return SocketAddress{
        .addrlen = addrlen,
        .addr = addr,
    };
}

void addSpaceIfNotEmpty(std::string& str) {
    if (!str.empty()) {
        str += " ";
    }
}

} // namespace

Listener::Listener(FileDescriptor listen_fd, SocketAddress local_address)
    : listen_fd_(std::move(listen_fd)),
      local_address_(local_address) {}

std::expected<Listener, ListenError> Listener::create(const std::string& interface, uint16_t port) {
    auto resolve_result = resolve(interface, port, true);
    if (!resolve_result) {
        return std::unexpected(ListenError{std::format("resolve {}:{} failed: {}", interface, port,
                                                       resolve_result.error().message)});
    }

    if (resolve_result->empty()) {
        return std::unexpected(
            ListenError{std::format("resolve {}:{} returned no addresses", interface, port)});
    }

    std::string attempts;
    for (const auto& resolved_address : *resolve_result) {
        std::string addr_str = formatAddress(resolved_address);

        auto socket_create_result = createTcpSocket(resolved_address);
        if (!socket_create_result) {
            addSpaceIfNotEmpty(attempts);
            attempts +=
                std::format("[{} socket: {}]", addr_str, socket_create_result.error().message());
            continue;
        }
        FileDescriptor socket_fd = std::move(socket_create_result.value());

        auto set_reuseaddr_result = setReuseAddress(socket_fd.get());
        if (!set_reuseaddr_result) {
            addSpaceIfNotEmpty(attempts);
            attempts += std::format("[{} setsockopt: {}]", addr_str,
                                    set_reuseaddr_result.error().message());
            continue;
        }

        auto bind_address_result = bindAddress(socket_fd.get(), resolved_address);
        if (!bind_address_result) {
            addSpaceIfNotEmpty(attempts);
            attempts +=
                std::format("[{} bind: {}]", addr_str, bind_address_result.error().message());
            continue;
        }

        auto enter_listen_result = enterListenState(socket_fd.get());
        if (!enter_listen_result) {
            addSpaceIfNotEmpty(attempts);
            attempts +=
                std::format("[{} listen: {}]", addr_str, enter_listen_result.error().message());
            continue;
        }

        auto local_address_result = getSocketLocalAddress(socket_fd.get());
        if (!local_address_result) {
            addSpaceIfNotEmpty(attempts);
            attempts += std::format("[{} getsockname: {}]", addr_str,
                                    local_address_result.error().message());
            continue;
        }

        return Listener(std::move(socket_fd), local_address_result.value());
    }

    return std::unexpected(
        ListenError(std::format("listen {}:{} failed: {}", interface, port, attempts)));
}

std::expected<AcceptSuccess, AcceptError> Listener::acceptClientConnection() {
    sockaddr_storage client_addr = {};
    socklen_t client_addr_len = sizeof(client_addr);

    int raw_fd = accept4(listen_fd_.get(), reinterpret_cast<sockaddr*>(&client_addr),
                         &client_addr_len, SOCK_CLOEXEC);

    if (raw_fd == -1) {
        return std::unexpected(
            AcceptError{std::format("accept: {}", std::system_category().message(errno))});
    }

    SocketAddress remote = {
        .addrlen = client_addr_len,
        .addr = client_addr,
    };

    return AcceptSuccess{
        .fd = FileDescriptor(raw_fd),
        .remote = remote,
    };
}

} // namespace orbit::net
