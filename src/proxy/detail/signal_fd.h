#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createShutdownSignalFd();
std::expected<int, std::error_code> drainSignalFd(int fd);

} // namespace orbit::proxy::detail
