#pragma once

#include <expected>
#include <system_error>

namespace orbit {

std::expected<void, std::error_code> runProxy(int downstream_fd, int upstream_fd);

} // namespace orbit
