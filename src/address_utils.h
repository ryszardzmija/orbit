#pragma once

#include <string>

#include <netinet/in.h>

namespace orbit {

std::string getIpv4AddressStr(in_addr_t address);

}