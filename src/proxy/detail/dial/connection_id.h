#pragma once

#include "proxy/detail/id/generator.h"

namespace orbit::proxy::detail {

using PendingConnectionId = MonotonicIdGenerator::Id;

class PendingConnectionIdGenerator {
public:
    PendingConnectionIdGenerator() = default;

    detail::PendingConnectionId getNextId() { return generator_.getNextId(); }

private:
    detail::MonotonicIdGenerator generator_;
};

} // namespace orbit::proxy::detail
