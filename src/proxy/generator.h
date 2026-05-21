#pragma once

#include <cstdint>

namespace orbit {

class MonotonicIdGenerator {
public:
    using Id = uint64_t;

    MonotonicIdGenerator() = default;

    Id getNextId() { return next_id_++; }

private:
    Id next_id_ = 1;
};

} // namespace orbit
