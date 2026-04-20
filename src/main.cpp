#include <cstdlib>
#include <iostream>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include "address_utils.h"
#include "connection.h"
#include "fd.h"
#include "proxy/proxy.h"
#include "socket_utils.h"

int main() {
    std::cout << "Starting proxy...\n";

    auto socket_result = orbit::createBlockingTcpSocket();
    if (!socket_result) {
        std::cerr << socket_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    orbit::FileDescriptor socket = std::move(socket_result.value());

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

    auto listen_result = orbit::enterListenState(socket.get());
    if (!listen_result) {
        std::cerr << listen_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Listening for downstream connections...\n";

    auto connection_result = orbit::acceptClientConnection(socket.get());
    if (!connection_result) {
        std::cerr << connection_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    orbit::Connection connection = std::move(connection_result.value());
    std::cout << "Client (" << orbit::getIpv4AddressStr(connection.address()) << ":"
              << connection.port() << ") connected\n";

    auto backend_socket_result = orbit::createBlockingTcpSocket();
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

    if (auto result = orbit::makeSocketNonBlocking(backend_socket.get()); !result) {
        std::cerr << result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Starting proxying traffic...\n";

    auto proxy_result = orbit::runProxy(connection.socketFd(), backend_socket.get());
    if (!proxy_result) {
        std::cerr << proxy_result.error().message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Proxy shutting down...\n";

    return EXIT_SUCCESS;
}
