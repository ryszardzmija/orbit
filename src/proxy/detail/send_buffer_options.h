#pragma once

#include <cstddef>

namespace orbit::proxy::detail {

struct SendBufferOptions {
    size_t block_size;
    size_t high_watermark;
    size_t low_watermark;
};

} // namespace orbit::proxy::detail
