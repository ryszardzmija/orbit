#pragma once

#include <string>

#include "net/socket_address.h"

namespace orbit::net {

std::string formatAddress(const SocketAddress& addr);

}
