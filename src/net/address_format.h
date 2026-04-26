#pragma once

#include <string>

#include "resolver.h"

namespace orbit::net {

std::string formatAddress(const ResolvedAddress& addr);

}
