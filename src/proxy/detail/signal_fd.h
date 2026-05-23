#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createShutdownSignalFd();

} // namespace orbit::proxy::detail
