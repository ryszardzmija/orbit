#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include "address_utils.h"
#include "common/fd.h"
#include "config/config.h"
#include "connection.h"
#include "logging/logger.h"
#include "proxy/proxy.h"
#include "socket_utils.h"

int main(int argc, char** argv) {
    auto config_result = orbit::parseConfig(argc, argv);
    if (!config_result) {
        return config_result.error().exit_code;
    }
    orbit::Config config = config_result.value();

    auto logger_guard = orbit::setUpLogger(config.log_level);

    spdlog::info("Starting proxy...");

    auto socket_result = orbit::createBlockingTcpSocket();
    if (!socket_result) {
        spdlog::error("socket() failed: {}", socket_result.error().message());
        return EXIT_FAILURE;
    }

    orbit::FileDescriptor socket = std::move(socket_result.value());

    auto reuse_flag_result = orbit::setReuseAddressFlag(socket.get());
    if (!reuse_flag_result) {
        spdlog::error("setsockopt() failed: {}", reuse_flag_result.error().message());
        return EXIT_FAILURE;
    }

    constexpr int binding_port = 8080;

    auto bind_result = orbit::bindAddress(socket.get(), binding_port);
    if (!bind_result) {
        spdlog::error("bind() failed: {}", bind_result.error().message());
        return EXIT_FAILURE;
    }

    auto listen_result = orbit::enterListenState(socket.get());
    if (!listen_result) {
        spdlog::error("listen() failed: {}", listen_result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Listening for downstream connections...");

    auto connection_result = orbit::acceptClientConnection(socket.get());
    if (!connection_result) {
        spdlog::error("accept() failed: {}", connection_result.error().message());
        return EXIT_FAILURE;
    }

    orbit::Connection connection = std::move(connection_result.value());
    spdlog::info("Client ({}:{}) connected", orbit::getIpv4AddressStr(connection.address()),
                 connection.port());

    auto backend_socket_result = orbit::createBlockingTcpSocket();
    if (!backend_socket_result) {
        spdlog::error("socket() failed: {}", backend_socket_result.error().message());
        return EXIT_FAILURE;
    }

    orbit::FileDescriptor backend_socket = std::move(backend_socket_result.value());

    constexpr in_port_t remote_port = 9000;
    const std::string remote_ip_addr("127.0.0.1");

    auto addr_conv_result = orbit::getIpv4AddressBin(remote_ip_addr);
    if (!addr_conv_result) {
        spdlog::error("inet_pton() failed: {}", addr_conv_result.error().message());
        return EXIT_FAILURE;
    }

    auto backend_connect_result =
        orbit::connectToRemote(backend_socket.get(), addr_conv_result.value(), remote_port);
    if (!backend_connect_result) {
        spdlog::error("connect() failed: {}", backend_connect_result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Connected to backend ({}:{})", remote_ip_addr, remote_port);

    if (auto result = orbit::makeSocketNonBlocking(backend_socket.get()); !result) {
        spdlog::error("fcntl() failed: {}", result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Starting proxying traffic...");

    auto proxy_result = orbit::runProxy(connection.socketFd(), backend_socket.get());
    if (!proxy_result) {
        spdlog::error("Forwarding failed: {}", proxy_result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Proxy shutting down...");

    return EXIT_SUCCESS;
}
