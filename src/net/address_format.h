#pragma once

#include <string>

#include "net/socket_address.h"

namespace orbit::net {

// Formats `socket_address` as a numeric host:port string.
// IPv6 addresses are formatted as [host]:port. Returns "<unknown address>"
// if the address cannot be formatted.
std::string formatAddress(const SocketAddress& socket_address);

} // namespace orbit::net
