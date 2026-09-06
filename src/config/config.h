#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <spdlog/common.h>

namespace orbit {

struct UpstreamConfig {
    std::string host;
    uint16_t port = 0;
};

struct SendBufferConfig {
    uint32_t max_buffer_size = 64 * 1024;
};

struct Config {
    std::string listen_host = "0.0.0.0";
    uint16_t listen_port = 8080;

    std::vector<UpstreamConfig> upstreams;
    spdlog::level::level_enum log_level = spdlog::level::level_enum::info;
    SendBufferConfig send_buffer;
};

struct ConfigError {
    int exit_code;
};

std::expected<Config, ConfigError> parseConfig(int argc, char** argv);

} // namespace orbit
