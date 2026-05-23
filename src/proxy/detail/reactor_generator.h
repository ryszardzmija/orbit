#pragma once

#include "proxy/detail/generator.h"
#include "proxy/detail/reactor_types.h"

namespace orbit::proxy::detail {

class SessionIdGenerator {
public:
    SessionIdGenerator() = default;

    detail::SessionId getNextId() { return generator_.getNextId(); }

private:
    detail::MonotonicIdGenerator generator_;
};

class ReactorSourceIdGenerator {
public:
    ReactorSourceIdGenerator() = default;

    detail::ReactorSourceId getNextId() { return generator_.getNextId(); }

private:
    detail::MonotonicIdGenerator generator_;
};

} // namespace orbit::proxy::detail
