#pragma once

#include "proxy/detail/deadlines.h"

namespace orbit::proxy::test {

class ControlledClock {
public:
    explicit ControlledClock(detail::Deadline now);
    ~ControlledClock();

    ControlledClock(const ControlledClock&) = delete;
    ControlledClock& operator=(const ControlledClock&) = delete;

    void advanceTo(detail::Deadline now) noexcept;
};

} // namespace orbit::proxy::test
