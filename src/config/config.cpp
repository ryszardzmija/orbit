#include "config/config.h"

#include <array>
#include <charconv>
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

} // namespace

std::expected<Config, ConfigError> parseConfig(int argc, char** argv) {
    Config config;
    std::vector<std::string> upstream_values;

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

    try {
        app.parse(argc, argv);

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
