#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"
#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createTimerFd();
std::expected<void, std::error_code> setTimer(int fd, std::optional<Deadline> at);
std::expected<bool, std::error_code> drainTimerFd(int fd);

} // namespace orbit::proxy::detail
