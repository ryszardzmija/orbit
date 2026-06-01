#pragma once

#include "proxy/detail/id/generator.h"

namespace orbit::proxy::detail {

using SourceId = MonotonicIdGenerator::Id;

class SourceIdGenerator {
public:
    detail::SourceId getNextId() { return generator_.getNextId(); }

private:
    detail::MonotonicIdGenerator generator_;
};

}; // namespace orbit::proxy::detail
