#include "config.h"

#include <array>
#include <limits>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <spdlog/common.h>

namespace orbit {

namespace {

const std::array<std::pair<std::string, spdlog::level::level_enum>, 7> level_map{{
    {"trace", spdlog::level::trace},
    {"debug", spdlog::level::debug},
    {"info", spdlog::level::info},
    {"warn", spdlog::level::warn},
    {"error", spdlog::level::err},
    {"critical", spdlog::level::critical},
    {"off", spdlog::level::off},
}};

constexpr int max_port_number = std::numeric_limits<uint16_t>::max();

} // namespace

std::expected<Config, ConfigError> parseConfig(int argc, char** argv) {
    Config config;
    CLI::App app("orbit - a TCP reverse proxy");
    app.set_version_flag("--version", "orbit 0.1.0");

    app.add_option("--listen-host", config.listen_host,
                   "Local IPv4 address to listen on (use 0.0.0.0 to listen on all interfaces)")
        ->envname("ORBIT_LISTEN_HOST")
        ->capture_default_str();

    app.add_option("--listen-port", config.listen_port,
                   "Port to listen on for incoming connections")
        ->envname("ORBIT_LISTEN_PORT")
        ->check(CLI::Range(1, max_port_number))
        ->capture_default_str();

    app.add_option("--upstream-host", config.upstream_host,
                   "Upstream server hostname or IPv4 address")
        ->required()
        ->envname("ORBIT_UPSTREAM_HOST");

    app.add_option("--upstream-port", config.upstream_port, "Upstream server port")
        ->required()
        ->envname("ORBIT_UPSTREAM_PORT")
        ->check(CLI::Range(1, max_port_number));

    app.add_option("--log-level", config.log_level, "Logging verbosity")
        ->envname("ORBIT_LOG_LEVEL")
        ->transform(CLI::CheckedTransformer(level_map, CLI::ignore_case))
        ->default_str("info");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return std::unexpected(ConfigError(app.exit(e)));
    }

    return config;
}

} // namespace orbit
