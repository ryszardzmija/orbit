#include "socket_utils.h"

#include <cerrno>

#include <netinet/in.h>
#include <sys/socket.h>

#include "connection.h"
#include "fd.h"

namespace orbit {

std::expected<FileDescriptor, std::error_code> createTcpSocket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

std::expected<void, std::error_code> bindAddress(int socket_fd, int port) {
    sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
        .sin_zero = {},
    };

    int result = bind(socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> enterListenState(int socket_fd) {
    constexpr int backlog_size = 10;

    int result = listen(socket_fd, backlog_size);
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<Connection, std::error_code> acceptClientConnection(int listening_socket) {
    sockaddr_in client_addr = {};
    socklen_t client_addr_size = sizeof(client_addr);

    int conn_fd =
        accept(listening_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_size);
    if (conn_fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return Connection(conn_fd, client_addr);
}

std::expected<void, std::error_code> setReuseAddressFlag(int socket_fd) {
    int opt = 1;
    int result = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

} // namespace orbit