#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "proxy/detail/allocation.h"

namespace orbit::proxy::detail {

using DeadlineClock = std::chrono::steady_clock;
using Deadline = DeadlineClock::time_point;

enum class DeadlineKind { Shutdown, Connect, Idle };

struct DeadlineKey {
    DeadlineKind kind;
    uint64_t id;
    auto operator<=>(const DeadlineKey&) const = default;
};

// At most one ordered entry per live key. The index also supports immediate cancellation.
class Deadlines {
public:
    explicit Deadlines(AllocationChecks allocation = {})
        : allocation_(allocation) {}
    void schedule(DeadlineKey key, Deadline at);
    void cancel(DeadlineKey key);
    std::optional<Deadline> scheduledAt(DeadlineKey key) const;
    bool due(DeadlineKey key, Deadline now) const;
    std::optional<Deadline> next() const;
    std::optional<DeadlineKey> popDue(Deadline now);

private:
    AllocationChecks allocation_;
    std::map<DeadlineKey, Deadline> times_;
    std::set<std::pair<Deadline, DeadlineKey>> ordered_;
};

} // namespace orbit::proxy::detail
