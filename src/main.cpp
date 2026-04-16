#include <expected>
#include <iostream>
#include <string>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

std::expected<int, std::error_code> createTcpSocket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return fd;
}

std::expected<void, std::error_code> bindAddress(int socket_fd, int port) {
    sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
        .sin_zero = {},
    };

    int result = bind(socket_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
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

struct Connection {
    int fd;
    in_addr_t address;
    in_port_t port;
};

std::expected<Connection, std::error_code> acceptClientConnection(int listening_socket) {
    sockaddr_in client_addr = {};
    socklen_t client_addr_size = sizeof(client_addr);

    int conn_fd =
        accept(listening_socket, reinterpret_cast<sockaddr *>(&client_addr), &client_addr_size);
    if (conn_fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return Connection{
        .fd = conn_fd,
        .address = ntohl(client_addr.sin_addr.s_addr),
        .port = ntohs(client_addr.sin_port),
    };
}

std::expected<void, std::error_code> handleClientConnection(int conn_fd) {
    // Read bytes until the client closes their end of TCP connection.
    constexpr size_t buf_size = 4096;
    char buf[buf_size];

    while (true) {
        ssize_t bytes_read = recv(conn_fd, buf, buf_size, 0);

        // Client closed their end of TCP connection.
        if (bytes_read == 0) {
            return {};
        }

        if (bytes_read == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        std::cout.write(buf, bytes_read);
    }
}

std::expected<void, std::error_code> setReuseAddressFlag(int socket_fd) {
    int opt = 1;
    int result = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::string getIpAddressStr(in_addr_t address) {
    char addr_str[INET_ADDRSTRLEN];
    in_addr addr_buf = {.s_addr = htonl(address)};
    inet_ntop(AF_INET, &addr_buf, addr_str, sizeof(addr_str));

    return std::string(addr_str);
}

// NOTE: On failure main() leaks a file descriptor. Until sockets are wrapped in RAII types
// it is acceptable.
int main() {
    auto socket_result = createTcpSocket();
    if (!socket_result) {
        std::cerr << socket_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    int socket_fd = socket_result.value();

    std::cout << "Socket successfully created: " << socket_fd << '\n';

    auto reuse_flag_result = setReuseAddressFlag(socket_fd);
    if (!reuse_flag_result) {
        std::cerr << reuse_flag_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    constexpr int binding_port = 8080;

    auto bind_result = bindAddress(socket_fd, binding_port);
    if (!bind_result) {
        std::cerr << bind_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Socket successfully bound to port " << binding_port << '\n';

    auto listen_result = enterListenState(socket_fd);
    if (!listen_result) {
        std::cerr << listen_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Socket successfully transitioned into listening state\n";

    auto connection_result = acceptClientConnection(socket_fd);
    if (!connection_result) {
        std::cerr << connection_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    auto [conn_fd, client_addr, client_port] = connection_result.value();
    std::cout << "Client (" << getIpAddressStr(client_addr) << ", " << client_port
              << ") connected\n";

    auto handling_result = handleClientConnection(conn_fd);
    if (!handling_result) {
        std::cerr << handling_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    close(conn_fd);
    close(socket_fd);
    return EXIT_SUCCESS;
}
