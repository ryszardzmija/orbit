#include "resolver.h"

#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <system_error>

#include <netdb.h>

namespace orbit::net {

namespace {

int getAiFlags(bool passive) {
    int flags = AI_ADDRCONFIG | AI_NUMERICSERV;

    if (passive) {
        flags |= AI_PASSIVE;
    }

    return flags;
}

addrinfo getHints(bool passive) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = getAiFlags(passive);

    return hints;
}

std::vector<ResolvedAddress> getAddressVector(addrinfo* resolve_result) {
    std::vector<ResolvedAddress> result;

    for (addrinfo* p = resolve_result; p != nullptr; p = p->ai_next) {
        ResolvedAddress entry = {};
        entry.addrlen = p->ai_addrlen;
        std::memcpy(&entry.addr, p->ai_addr, p->ai_addrlen);
        result.push_back(entry);
    }

    return result;
}

} // namespace

std::expected<std::vector<ResolvedAddress>, ResolveError> resolve(const std::string& hostname,
                                                                  uint16_t port, bool passive) {
    addrinfo* resolve_result = nullptr;
    addrinfo hints = getHints(passive);

    std::string port_str = std::to_string(port);
    const char* host_str = hostname.empty() ? nullptr : hostname.c_str();

    if (int res = getaddrinfo(host_str, port_str.c_str(), &hints, &resolve_result); res != 0) {
        if (res == EAI_SYSTEM) {
            return std::unexpected(ResolveError{std::system_category().message(errno)});
        }
        return std::unexpected(ResolveError{gai_strerror(res)});
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(resolve_result, &freeaddrinfo);
    return getAddressVector(resolve_result);
}

} // namespace orbit::net
