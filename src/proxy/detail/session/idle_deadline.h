#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "common/steady_time_source.h"
#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {

// Carries an established session's idle deadline through one readiness notification.
// Socket handlers qualify activity at the point where they observe it. The reactor
// owns the scheduled entry and applies the resulting refresh or close.
class IdleDeadline {
public:
    IdleDeadline(Deadline expires_at, std::chrono::milliseconds interval,
                 const SteadyTimeSource& time_source)
        : expires_at_(expires_at),
          interval_(interval),
          time_source_(time_source) {}

    bool canStartIo() { return !expired_ && check(time_source_.now()); }

    bool recordTransfer(size_t bytes) {
        if (bytes == 0) {
            return !expired_;
        }
        const auto observed_at = time_source_.now();
        if (!check(observed_at)) {
            return false;
        }
        expires_at_ = observed_at + interval_;
        refreshed_ = true;
        return true;
    }

    bool expired() const { return expired_; }

    std::optional<Deadline> takeRefresh() {
        if (std::exchange(refreshed_, false)) {
            return expires_at_;
        }
        return std::nullopt;
    }

private:
    bool check(Deadline now) {
        expired_ = expired_ || now >= expires_at_;
        return !expired_;
    }

    Deadline expires_at_;
    std::chrono::milliseconds interval_;
    const SteadyTimeSource& time_source_;
    bool expired_ = false;
    bool refreshed_ = false;
};

} // namespace orbit::proxy::detail
