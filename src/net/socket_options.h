#pragma once

#include <expected>
#include <system_error>

namespace orbit::net {

std::expected<void, std::error_code> setNonBlocking(int socket_fd);
std::expected<void, std::error_code> setReuseAddress(int socket_fd);

} // namespace orbit::net
