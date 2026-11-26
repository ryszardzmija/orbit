#include "config/config.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
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

struct UpstreamParseError {
    std::string message;
};

// parseUpstream() validates endpoint syntax and parses input into a host:port pair.
// It does not check whether hostname exists or whether IP address is valid.
std::expected<UpstreamConfig, UpstreamParseError> parseUpstream(std::string_view input) {
    std::string_view host;
    std::string_view port_text;

    if (input.starts_with('[')) {
        const std::size_t closing_bracket = input.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket + 1 >= input.size() ||
            input[closing_bracket + 1] != ':') {
            return std::unexpected(
                UpstreamParseError{"bracketed addresses must use [ADDRESS]:PORT"});
        }

        host = input.substr(1, closing_bracket - 1);
        if (host.find_first_of("[]") != std::string_view::npos) {
            return std::unexpected(
                UpstreamParseError{"bracketed addresses must use [ADDRESS]:PORT"});
        }
        port_text = input.substr(closing_bracket + 2);
    } else {
        if (input.find_first_of("[]") != std::string_view::npos) {
            return std::unexpected(
                UpstreamParseError{"bracketed addresses must use [ADDRESS]:PORT"});
        }

        const std::size_t separator = input.find(':');
        if (separator == std::string_view::npos) {
            return std::unexpected(UpstreamParseError{"expected HOST:PORT"});
        }
        if (input.find(':', separator + 1) != std::string_view::npos) {
            return std::unexpected(
                UpstreamParseError{"IPv6 addresses must use [ADDRESS]:PORT"});
        }

        host = input.substr(0, separator);
        port_text = input.substr(separator + 1);
    }

    if (host.empty()) {
        return std::unexpected(UpstreamParseError{"host must not be empty"});
    }
    if (port_text.empty()) {
        return std::unexpected(UpstreamParseError{"port must not be empty"});
    }

    unsigned int port = 0;
    const char* const port_end = port_text.data() + port_text.size();
    const auto [parsed_until, error] =
        std::from_chars(port_text.data(), port_end, port);

    if (error == std::errc::invalid_argument || parsed_until != port_end) {
        return std::unexpected(UpstreamParseError{"port must be a decimal integer"});
    }
    if (error == std::errc::result_out_of_range || port == 0 || port > max_port_number) {
        return std::unexpected(UpstreamParseError{"port must be between 1 and 65535"});
    }

    return UpstreamConfig{
        .host = std::string(host),
        .port = static_cast<uint16_t>(port),
    };
}

std::chrono::milliseconds parseTimeout(const std::string& text, const char* option,
                                       uint32_t minimum_ms) {
    uint32_t ms = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ms);
    if (ec != std::errc{} || end != text.data() + text.size() || ms < minimum_ms ||
        ms > TimeoutConfig::maximum.count()) {
        throw CLI::ValidationError(option, "expected decimal milliseconds between " +
                                               std::to_string(minimum_ms) + " and " +
                                               std::to_string(TimeoutConfig::maximum.count()));
    }
    return std::chrono::milliseconds(ms);
}

size_t parseResource(const CLI::Option& option, std::string_view text, size_t minimum,
                     size_t maximum) {
    // CLI11 skips empty environment values. A supplied resource value, even empty, is validated.
    if (option.count() == 0) {
        if (const char* supplied = std::getenv(option.get_envname().c_str())) {
            text = supplied;
        }
    }
    size_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum ||
        value > maximum) {
        throw CLI::ValidationError(option.get_name(), "expected a decimal integer between " +
                                                          std::to_string(minimum) + " and " +
                                                          std::to_string(maximum));
    }
    return value;
}

} // namespace

std::expected<Config, ConfigError> parseConfig(int argc, char** argv) {
    Config config;
    std::vector<std::string> upstream_values;

    auto connect_ms = std::to_string(config.timeouts.connect.count());
    auto idle_ms = std::to_string(config.timeouts.idle.count());
    auto buffer_high = std::to_string(config.resources.buffer_high_watermark);
    auto buffer_low = std::to_string(config.resources.buffer_low_watermark);
    auto connection_limit = std::to_string(config.resources.max_connections);
    auto pending_limit = std::to_string(config.resources.max_pending_connections);

    CLI::App app("orbit - a TCP reverse proxy");
    app.set_version_flag("--version", "orbit 0.1.0");

    app.add_option(
           "--listen-host", config.listen_host,
           "Local IPv4/IPv6 address to listen on (use 0.0.0.0/:: to listen on all interfaces)")
        ->envname("ORBIT_LISTEN_HOST")
        ->capture_default_str();

    app.add_option("--listen-port", config.listen_port,
                   "Port to listen on for incoming connections")
        ->envname("ORBIT_LISTEN_PORT")
        ->check(CLI::Range(1, max_port_number))
        ->capture_default_str();

    app.add_option("--upstream", upstream_values,
                   "Upstream server as HOST:PORT; may be specified multiple times")
        ->envname("ORBIT_UPSTREAMS")
        ->delimiter(',')
        ->type_name("HOST:PORT")
        ->required();

    app.add_option("--log-level", config.log_level, "Logging verbosity")
        ->envname("ORBIT_LOG_LEVEL")
        ->transform(CLI::CheckedTransformer(level_map, CLI::ignore_case))
        ->default_str("info");

    app.add_option("--connect-timeout-ms", connect_ms,
                   "Overall upstream connection timeout in milliseconds")
        ->envname("ORBIT_CONNECT_TIMEOUT_MS")
        ->capture_default_str();

    app.add_option("--idle-timeout-ms", idle_ms,
                   "Established session idle timeout in milliseconds (0 disables)")
        ->envname("ORBIT_IDLE_TIMEOUT_MS")
        ->capture_default_str();

    const auto* high_option = app.add_option("--buffer-high-watermark-bytes", buffer_high,
                                             "Maximum queued payload bytes per direction")
                                  ->envname("ORBIT_BUFFER_HIGH_WATERMARK_BYTES")
                                  ->capture_default_str();
    const auto* low_option =
        app.add_option("--buffer-low-watermark-bytes", buffer_low,
                       "Resume paused reads at or below this queue size in bytes")
            ->envname("ORBIT_BUFFER_LOW_WATERMARK_BYTES")
            ->capture_default_str();
    const auto* connection_option =
        app.add_option("--max-connections", connection_limit,
                       "Maximum admitted pending and established connections combined")
            ->envname("ORBIT_MAX_CONNECTIONS")
            ->capture_default_str();
    const auto* pending_option =
        app.add_option("--max-pending-connections", pending_limit,
                       "Maximum admitted clients establishing upstream connections")
            ->envname("ORBIT_MAX_PENDING_CONNECTIONS")
            ->capture_default_str();

    try {
        app.parse(argc, argv);

        config.timeouts.connect = parseTimeout(connect_ms, "--connect-timeout-ms", 1);
        config.timeouts.idle = parseTimeout(idle_ms, "--idle-timeout-ms", 0);
        config.resources.buffer_high_watermark =
            parseResource(*high_option, buffer_high, ResourceConfig::payload_block_size,
                          ResourceConfig::max_directional_limit);
        config.resources.buffer_low_watermark =
            parseResource(*low_option, buffer_low, 0, config.resources.buffer_high_watermark - 1);
        config.resources.max_connections = parseResource(*connection_option, connection_limit, 1,
                                                         ResourceConfig::connection_limit_ceiling);
        config.resources.max_pending_connections =
            parseResource(*pending_option, pending_limit, 1, config.resources.max_connections);

        config.upstreams.reserve(upstream_values.size());
        for (const std::string& upstream_value : upstream_values) {
            auto upstream = parseUpstream(upstream_value);
            if (!upstream) {
                throw CLI::ValidationError("--upstream", upstream.error().message);
            }
            config.upstreams.push_back(std::move(upstream).value());
        }
    } catch (const CLI::ParseError& e) {
        return std::unexpected(ConfigError(app.exit(e)));
    }

    return config;
}

} // namespace orbit
