#pragma once

#include <cstddef>

namespace orbit {

struct ResourceConfig {
    static constexpr size_t payload_block_size = 4096;
    static constexpr size_t max_directional_limit = 16 * 1024 * 1024;

    size_t buffer_high_watermark = 256 * 1024;
    size_t buffer_low_watermark = 192 * 1024;

    constexpr bool validBufferWatermarks() const {
        return buffer_high_watermark >= payload_block_size &&
               buffer_high_watermark <= max_directional_limit &&
               buffer_low_watermark < buffer_high_watermark;
    }
};

} // namespace orbit
