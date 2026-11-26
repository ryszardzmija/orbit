#include "proxy/detail/upstream_dialer.h"

#include <cassert>
#include <expected>
#include <string>
#include <system_error>

#include <sys/socket.h>

#include "net/dialer.h"

namespace orbit::proxy::detail {

namespace {

std::expected<void, net::DialError> checkConnectCompletion(int fd,
                                                           const net::DialOperations& operations) {
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);

    const int result = operations.getsockopt
                           ? operations.getsockopt(operations.context, fd, SOL_SOCKET, SO_ERROR,
                                                   &socket_error, &error_size)
                           : getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size);
    if (result == -1) {
        return std::unexpected(net::DialError{std::error_code(errno, std::system_category()),
                                              net::DialFailedOp::CompletionQuery});
    }

    if (socket_error != 0) {
        return std::unexpected(net::DialError{std::error_code(socket_error, std::system_category()),
                                              net::DialFailedOp::Completion});
    }

    return {};
}

net::DialError timedOut() {
    return {std::make_error_code(std::errc::timed_out), net::DialFailedOp::Deadline};
}

} // namespace

UpstreamDialer::UpstreamDialer(detail::ResolvedUpstream resolved_upstream,
                               net::DialOperations operations)
    : resolved_upstream_(std::move(resolved_upstream)),
      operations_(operations) {}

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

std::expected<UpstreamDialResult, net::DialError>
UpstreamDialer::dial(FileDescriptor accepted_connection, const net::SocketAddress& accepted_address,
                     Deadline deadline, const SteadyTimeSource& time_source,
                     size_t& attempts_left) {
    assert(resolved_upstream_.candidates().size() > 0);

    PendingConnection pending_connection{
        .accepted_connection_fd = std::move(accepted_connection),
        .accepted_address = accepted_address,
        .attempted_connection_fd = std::nullopt,
        .attempted_address_idx = 0,
        .deadline = deadline,
    };

    return tryCandidatesFrom(std::move(pending_connection), 0, time_source, attempts_left);
}

std::expected<UpstreamDialResult, net::DialError>
UpstreamDialer::advanceDialAttempt(PendingConnection pending_connection,
                                   const SteadyTimeSource& time_source, size_t& attempts_left) {
    assert(pending_connection.attempted_connection_fd.has_value());

    if (time_source.now() >= pending_connection.deadline) {
        return std::unexpected(timedOut());
    }
    auto completion_result =
        checkConnectCompletion(pending_connection.attempted_connection_fd->get(), operations_);

    if (completion_result) {
        return UpstreamDialConnected{
            .accepted_connection_fd = std::move(pending_connection.accepted_connection_fd),
            .upstream_connection_fd = std::move(*pending_connection.attempted_connection_fd),
            .accepted_address = pending_connection.accepted_address,
            .upstream_address =
                resolved_upstream_.candidates()[pending_connection.attempted_address_idx],
            .deadline = pending_connection.deadline,
        };
    }

    if (completion_result.error().failed_op == net::DialFailedOp::CompletionQuery &&
        completion_result.error().code.value() == EINTR) {
        return UpstreamDialDeferred{std::move(pending_connection), true};
    }
    size_t next_idx = pending_connection.attempted_address_idx + 1;
    if (next_idx >= resolved_upstream_.candidates().size() ||
        net::classifyDialError(completion_result.error()) != net::DialFailureKind::Candidate) {
        return std::unexpected(completion_result.error());
    }

    // The failed asynchronous attempt must no longer remain owned by pending connection.
    pending_connection.attempted_connection_fd.reset();

    return tryCandidatesFrom(std::move(pending_connection), next_idx, time_source, attempts_left);
}

std::expected<UpstreamDialResult, net::DialError>
UpstreamDialer::resumeDial(PendingConnection pending_connection,
                           const SteadyTimeSource& time_source, size_t& attempts_left) {
    const size_t next_idx = pending_connection.attempted_address_idx;
    return tryCandidatesFrom(std::move(pending_connection), next_idx, time_source, attempts_left);
}

const net::ResolutionEndpoint& UpstreamDialer::upstreamEndpoint() const {
    return resolved_upstream_.configuredEndpoint();
}

std::expected<UpstreamDialResult, net::DialError>
UpstreamDialer::tryCandidatesFrom(PendingConnection pending_connection,
                                  std::size_t first_candidate_idx,
                                  const SteadyTimeSource& time_source, size_t& attempts_left) {
    for (size_t i = first_candidate_idx; i < resolved_upstream_.candidates().size();) {
        if (time_source.now() >= pending_connection.deadline) {
            return std::unexpected(timedOut());
        }
        pending_connection.attempted_address_idx = i;
        if (attempts_left == 0) {
            return UpstreamDialDeferred{std::move(pending_connection)};
        }
        --attempts_left;

        auto dial_result = net::dial(resolved_upstream_.candidates()[i],
                                     pending_connection.deadline, time_source, operations_);
        if (!dial_result) {
            if ((dial_result.error().failed_op == net::DialFailedOp::Connect ||
                 dial_result.error().failed_op == net::DialFailedOp::Socket) &&
                dial_result.error().code.value() == EINTR) {
                if (attempts_left == 0) {
                    return UpstreamDialDeferred{std::move(pending_connection), true};
                }
                continue; // Retry the same address with a fresh socket and the remaining budget.
            }
            if (net::classifyDialError(dial_result.error()) != net::DialFailureKind::Candidate ||
                ++i == resolved_upstream_.candidates().size()) {
                return std::unexpected(dial_result.error());
            }
            continue;
        }

        net::DialSuccess success = std::move(dial_result.value());

        if (success.state == net::ConnectState::Connected) {
            return UpstreamDialConnected{
                .accepted_connection_fd = std::move(pending_connection.accepted_connection_fd),
                .upstream_connection_fd = std::move(success.fd),
                .accepted_address = pending_connection.accepted_address,
                .upstream_address = resolved_upstream_.candidates()[i],
                .deadline = pending_connection.deadline,
            };
        }

        pending_connection.attempted_connection_fd = std::move(success.fd);

        return UpstreamDialInProgress{
            .pending_connection = std::move(pending_connection),
        };
    }

    return std::unexpected(net::DialError{std::make_error_code(std::errc::host_unreachable),
                                          net::DialFailedOp::Connect});
}

} // namespace orbit::proxy::detail
