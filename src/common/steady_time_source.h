#pragma once

#include <chrono>

namespace orbit {

class SteadyTimeSource {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Read = TimePoint (*)(void*) noexcept;

    constexpr SteadyTimeSource() = default;

    // The caller keeps the context alive for the lifetime of the time source.
    constexpr SteadyTimeSource(void* context, Read read) noexcept
        : context_(context),
          read_(read) {}

    TimePoint now() const noexcept {
        return read_ ? read_(context_) : std::chrono::steady_clock::now();
    }

    bool usesSystemClock() const noexcept { return read_ == nullptr; }

private:
    void* context_ = nullptr;
    Read read_ = nullptr;
};

} // namespace orbit
