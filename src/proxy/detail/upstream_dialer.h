#pragma once

#include <expected>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

#include "common/steady_time_source.h"
#include "net/dialer.h"
#include "proxy/detail/pending_connection.h"
#include "proxy/detail/resolved_upstream.h"

namespace orbit::proxy::detail {

struct UpstreamDialConnected {
    FileDescriptor accepted_connection_fd;
    FileDescriptor upstream_connection_fd;
    net::SocketAddress accepted_address;
    net::SocketAddress upstream_address;
    Deadline deadline;
};

struct UpstreamDialInProgress {
    PendingConnection pending_connection;
};

struct UpstreamDialDeferred {
    PendingConnection pending_connection;
    bool interrupted = false;
};

using UpstreamDialResult =
    std::variant<UpstreamDialConnected, UpstreamDialInProgress, UpstreamDialDeferred>;

struct UpstreamDialerCreateError {
    std::string message;
};

class UpstreamDialer {
public:
    static std::expected<UpstreamDialer, UpstreamDialerCreateError>
    create(const net::ResolutionEndpoint& upstream_address);

    std::expected<UpstreamDialResult, net::DialError>
    dial(FileDescriptor accepted_connection, const net::SocketAddress& accepted_address,
         Deadline deadline, const SteadyTimeSource& time_source, size_t& attempts_left);
    std::expected<UpstreamDialResult, net::DialError>
    advanceDialAttempt(PendingConnection pending_connection, const SteadyTimeSource& time_source,
                       size_t& attempts_left);
    std::expected<UpstreamDialResult, net::DialError>
    resumeDial(PendingConnection pending_connection, const SteadyTimeSource& time_source,
               size_t& attempts_left);

    const net::ResolutionEndpoint& upstreamEndpoint() const;

private:
    friend class UpstreamDialerTest;
    explicit UpstreamDialer(detail::ResolvedUpstream resolved_upstream,
                            net::DialOperations operations = {});

    std::expected<UpstreamDialResult, net::DialError>
    tryCandidatesFrom(PendingConnection pending_connection, std::size_t first_candidate_idx,
                      const SteadyTimeSource& time_source, size_t& attempts_left);

    detail::ResolvedUpstream resolved_upstream_;
    net::DialOperations operations_;
};

} // namespace orbit::proxy::detail
