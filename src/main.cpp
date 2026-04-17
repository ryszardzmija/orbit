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

int main() {
    // Set up a socket in listening state and accept a client connection

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
    std::cout << "Client (" << orbit::getIpv4AddressStr(connection.address()) << ":"
              << connection.port() << ") connected\n";

    // Create a TCP connection to a backend
    auto backend_socket_result = orbit::createTcpSocket();
    if (!backend_socket_result) {
        std::cerr << backend_socket_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    orbit::FileDescriptor backend_socket = std::move(backend_socket_result.value());

    constexpr in_port_t remote_port = 9000;
    const std::string remote_ip_addr("127.0.0.1");

    auto addr_conv_result = orbit::getIpv4AddressBin(remote_ip_addr);
    if (!addr_conv_result) {
        std::cerr << addr_conv_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    auto backend_connect_result =
        orbit::connectToRemote(backend_socket.get(), addr_conv_result.value(), remote_port);
    if (!backend_connect_result) {
        std::cerr << backend_connect_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Connected to backend (" << remote_ip_addr << ":" << remote_port << ")\n";

    return EXIT_SUCCESS;
}
