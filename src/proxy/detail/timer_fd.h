#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createShutdownTimerFd();
std::expected<void, std::error_code> armTimer(int fd, int seconds);
std::expected<void, std::error_code> disarmTimer(int fd);
std::expected<void, std::error_code> drainTimerFd(int fd);

} // namespace orbit::proxy::detail
