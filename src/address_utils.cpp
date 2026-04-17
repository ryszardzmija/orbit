#include "address_utils.h"

#include <expected>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace orbit {

std::string getIpv4AddressStr(in_addr_t address) {
    char addr_str[INET_ADDRSTRLEN];
    // inet_ntop() requires the binary address to be in network byte-order
    in_addr addr_buf = {.s_addr = htonl(address)};
    inet_ntop(AF_INET, &addr_buf, addr_str, sizeof(addr_str));

    return std::string(addr_str);
}

std::expected<in_addr_t, std::error_code> getIpv4AddressBin(const std::string& address) {
    in_addr address_bin = {};
    int result = inet_pton(AF_INET, address.c_str(), &address_bin);
    if (result != 1) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    return ntohl(address_bin.s_addr);
}

} // namespace orbit
