#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "logging/logger.h"
#include "net/listener.h"
#include "proxy/proxy.h"
#include "signal/signal_block.h"

int main(int argc, char** argv) {
    auto signal_mask_make_result = orbit::makeShutdownSignalMask();
    if (!signal_mask_make_result) {
        return EXIT_FAILURE;
    }
    sigset_t shutdown_signal_mask = signal_mask_make_result.value();
    if (auto signal_block_result = orbit::blockSignals(shutdown_signal_mask);
        !signal_block_result) {
        return EXIT_FAILURE;
    }

    auto config_result = orbit::parseConfig(argc, argv);
    if (!config_result) {
        return config_result.error().exit_code;
    }
    orbit::Config config = config_result.value();

    auto logger_guard = orbit::setUpLogger(config.log_level);

    auto reactor_create_result = orbit::proxy::ProxyReactor::create(
        orbit::net::ListenSocketAddress{
            .interface = config.listen_host,
            .port = config.listen_port,
        },
        orbit::net::ResolutionEndpoint{
            .hostname = config.upstream_host,
            .port = config.upstream_port,
        });

    if (!reactor_create_result) {
        spdlog::error("Failed to initialize event loop: {}", reactor_create_result.error().message);
        return EXIT_FAILURE;
    }
    orbit::proxy::ProxyReactor reactor = std::move(reactor_create_result.value());

    if (auto event_loop_result = reactor.start(); !event_loop_result) {
        spdlog::error("Fatal event loop error: {}", event_loop_result.error().message);
        return EXIT_FAILURE;
    }

    spdlog::info("Proxy shutting down...");

    return EXIT_SUCCESS;
}
