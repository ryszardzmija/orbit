#pragma once

#include <expected>

#include "net/resolver.h"
#include "net/socket_address.h"

namespace orbit::proxy::detail {

struct ResolvedUpstreamCreateError {
    std::string message;
};

class ResolvedUpstream {
public:
    static std::expected<ResolvedUpstream, ResolvedUpstreamCreateError>
    create(const net::ResolutionEndpoint& configured_endpoint);

    const net::ResolutionEndpoint& configuredEndpoint() const;
    const std::vector<net::SocketAddress>& candidates() const;

private:
    ResolvedUpstream(const net::ResolutionEndpoint& configured_endpoint,
                     std::vector<net::SocketAddress> candidates);

    net::ResolutionEndpoint configured_endpoint_;
    std::vector<net::SocketAddress> candidates_;
};

} // namespace orbit::proxy::detail
