#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {

void Deadlines::schedule(DeadlineKey key, Deadline at) {
    auto [it, inserted] = times_.try_emplace(key, at);
    if (inserted) {
        try {
            allocation_.check(AllocationPoint::DeadlineIndex);
            ordered_.emplace(at, key);
        } catch (const std::bad_alloc&) {
            times_.erase(it);
            throw;
        }
    } else if (it->second != at) {
        auto entry = ordered_.extract({it->second, key});
        entry.value().first = at;
        ordered_.insert(std::move(entry));
        it->second = at;
    }
}

void Deadlines::cancel(DeadlineKey key) {
    auto it = times_.find(key);
    if (it != times_.end()) {
        ordered_.erase({it->second, key});
        times_.erase(it);
    }
}

std::optional<Deadline> Deadlines::scheduledAt(DeadlineKey key) const {
    auto it = times_.find(key);
    if (it == times_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Deadlines::due(DeadlineKey key, Deadline now) const {
    auto at = scheduledAt(key);
    return at && *at <= now;
}

std::optional<Deadline> Deadlines::next() const {
    if (ordered_.empty()) {
        return std::nullopt;
    }
    return ordered_.begin()->first;
}

std::optional<DeadlineKey> Deadlines::popDue(Deadline now) {
    auto it = ordered_.begin();
    if (it == ordered_.end() || it->first > now) {
        return std::nullopt;
    }
    auto key = it->second;
    ordered_.erase(it);
    times_.erase(key);
    return key;
}

} // namespace orbit::proxy::detail
