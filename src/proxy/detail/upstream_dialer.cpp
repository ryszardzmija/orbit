#include "proxy/detail/upstream_dialer.h"

#include <cassert>
#include <expected>
#include <format>
#include <string>
#include <system_error>

#include <sys/socket.h>

#include "net/address_format.h"
#include "net/dialer.h"

namespace orbit::proxy::detail {

namespace {

std::expected<void, std::error_code> checkConnectCompletion(int fd) {
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);

    if (int result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    if (socket_error != 0) {
        return std::unexpected(std::error_code(socket_error, std::system_category()));
    }

    return {};
}

} // namespace

UpstreamDialer::UpstreamDialer(detail::ResolvedUpstream resolved_upstream)
    : resolved_upstream_(std::move(resolved_upstream)) {}

std::expected<UpstreamDialer, UpstreamDialerCreateError>
UpstreamDialer::create(const net::ResolutionEndpoint& upstream_address) {

    auto resolved_upstream_create_result = detail::ResolvedUpstream::create(upstream_address);
    if (!resolved_upstream_create_result) {
        return std::unexpected(
            UpstreamDialerCreateError{resolved_upstream_create_result.error().message});
    }
    detail::ResolvedUpstream resolved_upstream = std::move(resolved_upstream_create_result.value());

    return UpstreamDialer(std::move(resolved_upstream));
}

std::expected<UpstreamDialResult, UpstreamDialError>
UpstreamDialer::dial(FileDescriptor accepted_connection,
                     const net::SocketAddress& accepted_address) {
    assert(resolved_upstream_.candidates().size() > 0);

    PendingConnection pending_connection{
        .accepted_connection_fd = std::move(accepted_connection),
        .accepted_address = accepted_address,
        .attempted_connection_fd = std::nullopt,
        .attempted_address_idx = 0,
        .error_messages = std::nullopt,
        .error_codes = std::nullopt,
    };

    return tryCandidatesFrom(std::move(pending_connection), 0);
}

std::expected<UpstreamDialResult, UpstreamDialError>
UpstreamDialer::advanceDialAttempt(PendingConnection pending_connection) {
    assert(pending_connection.attempted_connection_fd.has_value());

    auto completion_result =
        checkConnectCompletion(pending_connection.attempted_connection_fd->get());

    if (completion_result) {
        return UpstreamDialConnected{
            .accepted_connection_fd = std::move(pending_connection.accepted_connection_fd),
            .upstream_connection_fd = std::move(*pending_connection.attempted_connection_fd),
            .accepted_address = pending_connection.accepted_address,
            .upstream_address =
                resolved_upstream_.candidates()[pending_connection.attempted_address_idx],
        };
    }

    addError(pending_connection, net::DialFailedOp::Connect, completion_result.error());

    // The failed asynchronous attempt must no longer remain owned by pending connection.
    pending_connection.attempted_connection_fd.reset();

    size_t next_idx = pending_connection.attempted_address_idx + 1;
    return tryCandidatesFrom(std::move(pending_connection), next_idx);
}

const net::ResolutionEndpoint& UpstreamDialer::upstreamEndpoint() const {
    return resolved_upstream_.configuredEndpoint();
}

std::expected<UpstreamDialResult, UpstreamDialError>
UpstreamDialer::tryCandidatesFrom(PendingConnection pending_connection,
                                  std::size_t first_candidate_idx) {
    for (size_t i = first_candidate_idx; i < resolved_upstream_.candidates().size(); i++) {
        pending_connection.attempted_address_idx = i;

        auto dial_result = net::dial(resolved_upstream_.candidates()[i]);
        if (!dial_result) {
            addError(pending_connection, dial_result.error().failed_op, dial_result.error().code);

            // Failure to create a socket should be treated as fatal since retries
            // are unlikely to succeed.
            if (dial_result.error().failed_op == net::DialFailedOp::Socket) {
                break;
            }

            // Otherwise attempt to dial the next address.
            continue;
        }

        net::DialSuccess success = std::move(dial_result.value());

        if (success.state == net::ConnectState::Connected) {
            return UpstreamDialConnected{
                .accepted_connection_fd = std::move(pending_connection.accepted_connection_fd),
                .upstream_connection_fd = std::move(success.fd),
                .accepted_address = pending_connection.accepted_address,
                .upstream_address = resolved_upstream_.candidates()[i],
            };
        }

        pending_connection.attempted_connection_fd = std::move(success.fd);

        return UpstreamDialInProgress{
            .pending_connection = std::move(pending_connection),
        };
    }

    assert(pending_connection.error_messages.has_value());
    assert(pending_connection.error_codes.has_value());

    return std::unexpected(UpstreamDialError{
        .error_messages = std::move(*pending_connection.error_messages),
        .error_codes = std::move(*pending_connection.error_codes),
    });
}

void UpstreamDialer::addError(PendingConnection& pending_connection, net::DialFailedOp failed_op,
                              std::error_code error_code) {
    if (!pending_connection.error_messages) {
        pending_connection.error_messages = "";
    }
    if (!pending_connection.error_codes) {
        pending_connection.error_codes = std::vector<std::error_code>();
    }

    if (!pending_connection.error_messages->empty()) {
        *pending_connection.error_messages += " ";
    }

    switch (failed_op) {
    case net::DialFailedOp::Socket:
        *pending_connection.error_messages += std::format(
            "[{} socket: {}]",
            net::formatAddress(
                resolved_upstream_.candidates()[pending_connection.attempted_address_idx]),
            error_code.message());
        break;
    case net::DialFailedOp::Connect:
        *pending_connection.error_messages += std::format(
            "[{} connect: {}]",
            net::formatAddress(
                resolved_upstream_.candidates()[pending_connection.attempted_address_idx]),
            error_code.message());
    }

    pending_connection.error_codes->push_back(error_code);
}

} // namespace orbit::proxy::detail
