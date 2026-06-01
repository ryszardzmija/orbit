#pragma once

#include "proxy/detail/id/generator.h"

namespace orbit::proxy::detail {

using SessionId = MonotonicIdGenerator::Id;

class SessionIdGenerator {
public:
    detail::SessionId getNextId() { return generator_.getNextId(); }

private:
    detail::MonotonicIdGenerator generator_;
};

} // namespace orbit::proxy::detail
