#pragma once

#include <expected>
#include <system_error>

#include <signal.h> // IWYU pragma: keep

namespace orbit {

std::expected<sigset_t, std::error_code> makeShutdownSignalMask();
std::expected<void, std::error_code> blockSignals(sigset_t mask);

} // namespace orbit
