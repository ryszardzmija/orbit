#pragma once

#include <new>

namespace orbit::proxy::detail {

enum class AllocationPoint { SendBuffer, PayloadBlock, DeadlineIndex };

struct AllocationChecks {
    void* context = nullptr;
    bool (*reject)(void*, AllocationPoint) noexcept = nullptr;

    void check(AllocationPoint point) const {
        if (reject && reject(context, point)) {
            throw std::bad_alloc();
        }
    }
};

} // namespace orbit::proxy::detail
