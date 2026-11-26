#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>

#include "common/steady_time_source.h"
#include "proxy/detail/deadlines.h"

namespace orbit::proxy::test {

class ControlledClock {
public:
    explicit ControlledClock(detail::Deadline now = {})
        : now_(now) {}

    ControlledClock(const ControlledClock&) = delete;
    ControlledClock& operator=(const ControlledClock&) = delete;

    SteadyTimeSource source() {
        return {this, [](void* context) noexcept {
                    auto& clock = *static_cast<ControlledClock*>(context);
                    ++clock.reads_;
                    const auto now = clock.now_;
                    if (auto next = std::exchange(clock.after_read_, std::nullopt)) {
                        clock.advanceTo(*next);
                    }
                    return now;
                }};
    }

    detail::Deadline now() const { return now_; }
    size_t reads() const { return reads_; }

    void advanceTo(detail::Deadline now) noexcept {
        assert(now >= now_);
        now_ = now;
    }

    // Return the current time to one observer, then advance before the next observation.
    void advanceAfterNextRead(detail::Deadline now) {
        assert(now >= now_);
        after_read_ = now;
    }

private:
    detail::Deadline now_;
    size_t reads_ = 0;
    std::optional<detail::Deadline> after_read_;
};

} // namespace orbit::proxy::test
