#pragma once

#include <expected>
#include <string>
#include <system_error>

#include <netinet/in.h>

namespace orbit {

std::string getIpv4AddressStr(in_addr_t address);

std::expected<in_addr_t, std::error_code> getIpv4AddressBin(const std::string& address);

} // namespace orbit
