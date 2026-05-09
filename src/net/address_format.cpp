#include "address_format.h"

#include <format>
#include <string>

#include <netdb.h>

#include "net/socket_address.h"

namespace orbit::net {

std::string formatAddress(const SocketAddress& socket_address) {
    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    int res =
        getnameinfo(reinterpret_cast<const sockaddr*>(&socket_address.addr), socket_address.addrlen,
                    host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV);

    if (res != 0) {
        return "<unknown address>";
    }

    if (socket_address.addr.ss_family == AF_INET6) {
        return std::format("[{}]:{}", host, port);
    }

    return std::format("{}:{}", host, port);
}

} // namespace orbit::net
