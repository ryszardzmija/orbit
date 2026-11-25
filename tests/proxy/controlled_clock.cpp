#include "controlled_clock.h"

#include <cassert>
#include <optional>

namespace {

thread_local std::optional<orbit::proxy::detail::Deadline> controlled_now;

} // namespace

// Keep the C++ return type and give the functions the linker wrap's symbol names.
orbit::proxy::detail::Deadline realSteadyNow() noexcept
    asm("__real__ZNSt6chrono3_V212steady_clock3nowEv");
orbit::proxy::detail::Deadline wrappedSteadyNow() noexcept
    asm("__wrap__ZNSt6chrono3_V212steady_clock3nowEv");

orbit::proxy::detail::Deadline wrappedSteadyNow() noexcept {
    if (controlled_now) {
        return *controlled_now;
    }
    return realSteadyNow();
}

namespace orbit::proxy::test {

ControlledClock::ControlledClock(detail::Deadline now) {
    assert(!controlled_now);
    controlled_now = now;
}

ControlledClock::~ControlledClock() { controlled_now.reset(); }

void ControlledClock::advanceTo(detail::Deadline now) noexcept {
    assert(controlled_now && *controlled_now <= now);
    controlled_now = now;
}

} // namespace orbit::proxy::test
