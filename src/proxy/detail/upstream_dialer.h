#pragma once

#include <expected>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "net/dialer.h"
#include "proxy/detail/pending_connection.h"
#include "proxy/detail/resolved_upstream.h"

namespace orbit::proxy::detail {

struct UpstreamDialConnected {
    FileDescriptor accepted_connection_fd;
    FileDescriptor upstream_connection_fd;
    net::SocketAddress accepted_address;
    net::SocketAddress upstream_address;
};

struct UpstreamDialInProgress {
    PendingConnection pending_connection;
};

using UpstreamDialResult = std::variant<UpstreamDialConnected, UpstreamDialInProgress>;

struct UpstreamDialError {
    std::string error_messages;
    std::vector<std::error_code> error_codes;
};

struct UpstreamDialerCreateError {
    std::string message;
};

class UpstreamDialer {
public:
    static std::expected<UpstreamDialer, UpstreamDialerCreateError>
    create(const net::ResolutionEndpoint& upstream_address);

    std::expected<UpstreamDialResult, UpstreamDialError>
    dial(FileDescriptor accepted_connection, const net::SocketAddress& accepted_address);
    std::expected<UpstreamDialResult, UpstreamDialError>
    advanceDialAttempt(PendingConnection pending_connection);

    const net::ResolutionEndpoint& upstreamEndpoint() const;

private:
    explicit UpstreamDialer(detail::ResolvedUpstream resolved_upstream);

    void addError(PendingConnection& pending_connection, net::DialFailedOp failed_op,
                  std::error_code error_code);
    std::expected<UpstreamDialResult, UpstreamDialError>
    tryCandidatesFrom(PendingConnection pending_connection, std::size_t first_candidate_idx);

    detail::ResolvedUpstream resolved_upstream_;
};

} // namespace orbit::proxy::detail
