#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace orbit {

struct TimeoutConfig {
    static constexpr std::chrono::milliseconds maximum{std::numeric_limits<int32_t>::max()};
    std::chrono::milliseconds connect{std::chrono::seconds(10)};
};

} // namespace orbit
