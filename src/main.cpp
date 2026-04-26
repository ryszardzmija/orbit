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
#include "net/address_format.h"
#include "net/dialer.h"
#include "net/socket_options.h"
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

    auto upstream_dial_result = orbit::net::dial(config.upstream_host, config.upstream_port);
    if (!upstream_dial_result) {
        spdlog::error("Upstream connect error: {}", upstream_dial_result.error().message);
        return EXIT_FAILURE;
    }
    auto [upstream_socket_fd, remote] = std::move(upstream_dial_result.value());

    spdlog::info("Connected to upstream {}:{} ({})", config.upstream_host, config.upstream_port,
                 orbit::net::formatAddress(remote));

    if (auto result = orbit::net::setNonBlocking(upstream_socket_fd.get()); !result) {
        spdlog::error("fcntl() failed: {}", result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Starting proxying traffic...");

    auto proxy_result = orbit::runProxy(connection.socketFd(), upstream_socket_fd.get());
    if (!proxy_result) {
        spdlog::error("Forwarding failed: {}", proxy_result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Proxy shutting down...");

    return EXIT_SUCCESS;
}
