#pragma once

#include <cstddef>

namespace orbit {

struct Config {
    size_t block_size;
    size_t high_watermark;
    size_t low_watermark;
};

} // namespace orbit
