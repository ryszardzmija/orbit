#pragma once

#include <cstddef>

namespace orbit {

struct ResourceConfig {
    static constexpr size_t payload_block_size = 4096;
    static constexpr size_t max_directional_limit = 16 * 1024 * 1024;
    static constexpr size_t connection_limit_ceiling = 65535;
    static constexpr size_t default_buffer_budget = 64 * 1024 * 1024;
    static constexpr size_t max_buffer_budget = 1024 * 1024 * 1024;

    size_t buffer_high_watermark = 256 * 1024;
    size_t buffer_low_watermark = 192 * 1024;
    size_t max_connections = 256;
    size_t max_pending_connections = 64;
    size_t buffer_budget = default_buffer_budget;

    constexpr bool validBufferWatermarks() const {
        return buffer_high_watermark >= payload_block_size &&
               buffer_high_watermark <= max_directional_limit &&
               buffer_low_watermark < buffer_high_watermark;
    }

    constexpr bool validConnectionLimits() const {
        return max_connections > 0 && max_connections <= connection_limit_ceiling &&
               max_pending_connections > 0 && max_pending_connections <= max_connections;
    }

    constexpr bool validBufferBudget() const {
        return buffer_budget >= payload_block_size && buffer_budget <= max_buffer_budget &&
               buffer_budget % payload_block_size == 0;
    }
};

} // namespace orbit
