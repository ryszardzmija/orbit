#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include "address_utils.h"
#include "connection.h"
#include "fd.h"
#include "socket_utils.h"

namespace orbit {

std::expected<void, std::error_code> handleClientConnection(int socket_fd) {
    // Read bytes until the client closes their end of TCP connection.
    constexpr size_t buf_size = 4096;
    char buf[buf_size];

    while (true) {
        ssize_t bytes_read = recv(socket_fd, buf, buf_size, 0);

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

} // namespace orbit

int main() {
    auto socket_result = orbit::createTcpSocket();
    if (!socket_result) {
        std::cerr << socket_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    orbit::FileDescriptor socket = std::move(socket_result.value());

    std::cout << "Socket successfully created: " << socket.get() << '\n';

    auto reuse_flag_result = orbit::setReuseAddressFlag(socket.get());
    if (!reuse_flag_result) {
        std::cerr << reuse_flag_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    constexpr int binding_port = 8080;

    auto bind_result = orbit::bindAddress(socket.get(), binding_port);
    if (!bind_result) {
        std::cerr << bind_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Socket successfully bound to port " << binding_port << '\n';

    auto listen_result = orbit::enterListenState(socket.get());
    if (!listen_result) {
        std::cerr << listen_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Socket successfully transitioned into listening state\n";

    auto connection_result = orbit::acceptClientConnection(socket.get());
    if (!connection_result) {
        std::cerr << connection_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    orbit::Connection connection = std::move(connection_result.value());
    std::cout << "Client (" << orbit::getIpv4AddressStr(connection.address()) << ", "
              << connection.port() << ") connected\n";

    auto handling_result = orbit::handleClientConnection(connection.socketFd());
    if (!handling_result) {
        std::cerr << handling_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
