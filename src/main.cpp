#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "logging/logger.h"
#include "net/address_format.h"
#include "net/dialer.h"
#include "net/listener.h"
#include "net/socket_options.h"
#include "proxy/proxy.h"

int main(int argc, char** argv) {
    auto config_result = orbit::parseConfig(argc, argv);
    if (!config_result) {
        return config_result.error().exit_code;
    }
    orbit::Config config = config_result.value();

    auto logger_guard = orbit::setUpLogger(config.log_level);

    spdlog::info("Starting proxy...");

    auto listener_create_result =
        orbit::net::Listener::create(config.listen_host, config.listen_port);
    if (!listener_create_result) {
        spdlog::error("Listen socket init error: {}", listener_create_result.error().message);
        return EXIT_FAILURE;
    }
    orbit::net::Listener listener = std::move(listener_create_result.value());

    spdlog::info("Listening on {}", orbit::net::formatAddress(listener.localAddress()));

    auto accept_result = listener.acceptClientConnection();
    if (!accept_result) {
        spdlog::error("Client connection accept error: {}", accept_result.error().message);
        return EXIT_FAILURE;
    }

    auto [downstream_socket_fd, downstream_addr] = std::move(accept_result.value());

    spdlog::info("Accepted downstream connection from {}",
                 orbit::net::formatAddress(downstream_addr));

    auto upstream_dial_result = orbit::net::dial(config.upstream_host, config.upstream_port);
    if (!upstream_dial_result) {
        spdlog::error("Upstream connect error: {}", upstream_dial_result.error().message);
        return EXIT_FAILURE;
    }
    auto [upstream_socket_fd, upstream_addr] = std::move(upstream_dial_result.value());

    spdlog::info("Connected to upstream {}:{} ({})", config.upstream_host, config.upstream_port,
                 orbit::net::formatAddress(upstream_addr));

    if (auto result = orbit::net::setNonBlocking(downstream_socket_fd.get()); !result) {
        spdlog::error("fcntl() failed: {}", result.error().message());
        return EXIT_FAILURE;
    }
    if (auto result = orbit::net::setNonBlocking(upstream_socket_fd.get()); !result) {
        spdlog::error("fcntl() failed: {}", result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Starting proxying traffic...");

    auto reactor_create_result =
        orbit::ProxyReactor::create(downstream_socket_fd.get(), upstream_socket_fd.get());
    if (!reactor_create_result) {
        spdlog::error("Failed to initialize event loop: {}",
                      reactor_create_result.error().message());
        return EXIT_FAILURE;
    }
    orbit::ProxyReactor reactor = std::move(reactor_create_result.value());

    if (auto event_loop_result = reactor.start(); !event_loop_result) {
        spdlog::error("Forwarding failed: {}", event_loop_result.error().message());
        return EXIT_FAILURE;
    }

    spdlog::info("Proxy shutting down...");

    return EXIT_SUCCESS;
}
