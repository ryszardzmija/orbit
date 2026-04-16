#include "address_utils.h"

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

} // namespace orbit