#include "proxy/detail/resolved_upstream.h"

#include <format>

#include "net/resolver.h"

namespace orbit::proxy::detail {

ResolvedUpstream::ResolvedUpstream(const net::ResolutionEndpoint& configured_endpoint,
                                   std::vector<net::SocketAddress> candidates)
    : configured_endpoint_(configured_endpoint),
      candidates_(std::move(candidates)) {}

std::expected<ResolvedUpstream, ResolvedUpstreamCreateError>
ResolvedUpstream::create(const net::ResolutionEndpoint& configured_endpoint) {
    auto resolution_result = net::resolve(configured_endpoint, false);
    if (!resolution_result) {
        return std::unexpected(ResolvedUpstreamCreateError{resolution_result.error().message});
    }

    if (resolution_result->empty()) {
        return std::unexpected(ResolvedUpstreamCreateError{
            std::format("resolve {}:{} returned no addresses", configured_endpoint.hostname,
                        configured_endpoint.port)});
    }

    return ResolvedUpstream(configured_endpoint, std::move(resolution_result.value()));
}

const net::ResolutionEndpoint& ResolvedUpstream::configuredEndpoint() const {
    return configured_endpoint_;
}

const std::vector<net::SocketAddress>& ResolvedUpstream::candidates() const { return candidates_; }

} // namespace orbit::proxy::detail
