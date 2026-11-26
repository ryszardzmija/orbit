#include "proxy/detail/session/session_manager.h"

#include <cassert>
#include <cerrno>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <sys/socket.h>

#include "common/fd.h"
#include "common/status.h"
#include "proxy/detail/buffer/send_buffer.h"
#include "proxy/detail/session/session_id.h"
#include "proxy/detail/session/session_registration.h"
#include "proxy/detail/session/session_state.h"

namespace orbit::proxy::detail {

namespace {

Status<std::error_code> halfCloseIfReady(const SessionEndpoint& source,
                                         SessionEndpoint& destination, IdleDeadline* idle) {
    if (!destination.state.canWrite() || !source.state.done_reading ||
        !destination.state.send_buffer->empty()) {
        return {};
    }

    while (!idle || idle->canStartIo()) {
        if (shutdown(destination.fd.get(), SHUT_WR) == 0) {
            destination.state.half_close_sent = true;
            break;
        }
        if (errno != EINTR) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
    }
    return {};
}

bool shouldClose(const Session& session) {
    // Each destination has either received its ordered FIN or hung up.
    return !session.downstream.state.canWrite() && !session.upstream.state.canWrite();
}

SessionEventResult keepOpen() {
    return SessionEventResult{
        .action = SessionEventAction::KeepOpen,
        .error = std::nullopt,
    };
}

SessionEventResult closeSession(std::optional<std::string> error = std::nullopt) {
    return SessionEventResult{
        .action = SessionEventAction::Close,
        .error = std::move(error),
    };
}

} // namespace

SessionManager::SessionManager(const SessionManagerOptions& options,
                               net::StreamOperations operations, AllocationChecks allocation)
    : buffer_budget_(std::make_unique<BufferBudget>(options.buffer_budget)),
      block_size_(options.send_buffer.block_size),
      allocation_(allocation),
      send_buffer_factory_(options.send_buffer, allocation, *buffer_budget_),
      forwarder_(Forwarder::create(options.forwarder_buffer_capacity)),
      sender_(PendingDataSender::create(options.sender_buffer_capacity)),
      operations_(operations) {}

SessionId SessionManager::allocateId() { return session_id_generator_.getNextId(); }

std::optional<Session> SessionManager::prepare(FileDescriptor downstream,
                                               FileDescriptor upstream) const {
    try {
        return Session{
            .downstream = getInitialSessionEndpoint(std::move(downstream)),
            .upstream = getInitialSessionEndpoint(std::move(upstream)),
        };
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

void SessionManager::add(SessionId id, Session session, SessionSourceIds source_ids) {
    ManagedSession managed_session = {
        .session = std::move(session),
        .source_ids = source_ids,
    };

    auto [it, inserted] = sessions_.emplace(id, std::move(managed_session));
    assert(inserted && "session ID already added");
}

void SessionManager::remove(SessionId id) {
    auto& session = getSession(id);
    cancelReadWait(session.downstream);
    cancelReadWait(session.upstream);
    size_t erased = sessions_.erase(id);
    assert(erased == 1 && "session ID not found");
}

bool SessionManager::contains(SessionId id) const { return sessions_.contains(id); }

bool SessionManager::empty() const { return sessions_.empty(); }

bool SessionManager::hasReadyRead() const {
    return !waiting_reads_.empty() && buffer_budget_->available() >= block_size_;
}

std::optional<SourceId> SessionManager::takeReadyRead() {
    if (!hasReadyRead()) {
        return std::nullopt;
    }
    const auto [id, role] = waiting_reads_.front();
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        waiting_reads_.pop_front();
        return std::nullopt;
    }
    auto& managed = it->second;
    cancelReadWait(getEndpoint(managed.session, role));
    return role == EndpointRole::Downstream ? managed.source_ids.downstream
                                            : managed.source_ids.upstream;
}

void SessionManager::cancelReadWait(SessionEndpoint& endpoint) {
    if (auto wait = endpoint.state.capacity_wait) {
        waiting_reads_.erase(*wait);
        endpoint.state.capacity_wait.reset();
    }
}

SessionSourceIds SessionManager::sourceIds(SessionId id) const {
    return getManagedSession(id).source_ids;
}

SessionInterests SessionManager::interests(SessionId id) const {
    const Session& session = getSession(id);
    return SessionInterests{
        .downstream = getInterests(session.downstream, session.upstream),
        .upstream = getInterests(session.upstream, session.downstream),
    };
}

bool SessionManager::shouldRetire(SessionId id, EndpointRole role) const {
    const auto& endpoint = getEndpoint(getSession(id), role);
    return endpoint.state.hungUp() && endpoint.state.done_reading;
}

SessionEventResult SessionManager::handleReadable(SessionId id, EndpointRole role,
                                                  IdleDeadline* idle, bool from_wait_queue) {
    Session& session = getSession(id);
    SessionEndpoint& source = getEndpoint(session, role);
    SessionEndpoint& destination = getOtherEndpoint(session, role);

    if (idle && !idle->canStartIo()) {
        return closeSession();
    }
    if (!getInterests(source, destination).readable) {
        return keepOpen();
    }

    // While a waiter exists, newly ready directions may use tail space but cannot take a block.
    const bool allow_new_blocks = from_wait_queue || waiting_reads_.empty();
    auto result = forwarder_.forward(source, destination, idle, operations_, allow_new_blocks);
    if (!result) {
        if (result.error().failed_op == FailedOp::Buffer &&
            result.error().code == std::errc::not_enough_memory) {
            return closeSession();
        }
        return closeSession("Forwarding failed: " + result.error().code.message());
    }
    if (*result == ForwardOutcome::WaitForCapacity) {
        try {
            allocation_.check(AllocationPoint::WaitingRead);
            source.state.capacity_wait = waiting_reads_.insert(waiting_reads_.end(), {id, role});
        } catch (const std::bad_alloc&) {
            return closeSession();
        }
        return keepOpen();
    }

    if (auto result = halfCloseIfReady(source, destination, idle); !result) {
        return closeSession("Half-closing connection failed: " + result.error().message());
    }

    return shouldClose(session) ? closeSession() : keepOpen();
}

SessionEventResult SessionManager::handleWritable(SessionId id, EndpointRole role,
                                                  IdleDeadline* idle) {
    Session& session = getSession(id);
    SessionEndpoint& destination = getEndpoint(session, role);
    SessionEndpoint& source = getOtherEndpoint(session, role);

    if (idle && !idle->canStartIo()) {
        return closeSession();
    }
    if (!getInterests(destination, source).writable) {
        return keepOpen();
    }

    if (auto result = sender_.sendPending(destination, idle, operations_); !result) {
        return closeSession("Sending pending outbound data failed: " + result.error().message);
    }

    if (source.state.hungUp() && getInterests(source, destination).readable) {
        auto source_role =
            role == EndpointRole::Upstream ? EndpointRole::Downstream : EndpointRole::Upstream;
        return handleReadable(id, source_role, idle);
    }

    if (auto result = halfCloseIfReady(source, destination, idle); !result) {
        return closeSession("Half-closing connection failed: " + result.error().message());
    }

    return shouldClose(session) ? closeSession() : keepOpen();
}

SessionEventResult SessionManager::handlePeerHalfClosed(SessionId id, EndpointRole role,
                                                        IdleDeadline* idle) {
    if (idle && !idle->canStartIo()) {
        return closeSession();
    }
    getEndpoint(getSession(id), role).state.peer_half_closed = true;
    return keepOpen();
}

SessionEventResult SessionManager::handleHangup(SessionId id, EndpointRole role,
                                                IdleDeadline* idle) {
    Session& session = getSession(id);
    SessionEndpoint& hungup = getEndpoint(session, role);
    hungup.state.send_buffer.reset();
    cancelReadWait(getOtherEndpoint(session, role));
    hungup.state.peer_half_closed = true;
    if (shouldClose(session)) {
        return closeSession();
    }
    return handleReadable(id, role, idle);
}

SessionEventResult SessionManager::handleError(SessionId id, EndpointRole role) {
    const SessionEndpoint& endpoint = getEndpoint(getSession(id), role);
    int socket_error = 0;
    socklen_t len = sizeof(socket_error);

    if (int result = getsockopt(endpoint.fd.get(), SOL_SOCKET, SO_ERROR, &socket_error, &len);
        result == -1) {
        return closeSession("Failed to read socket error: " +
                            std::system_category().message(errno));
    }

    return closeSession("Socket error: " + std::system_category().message(socket_error));
}

SessionEndpoint SessionManager::getInitialSessionEndpoint(FileDescriptor fd) const {
    EndpointState endpoint_state = {
        .send_buffer = send_buffer_factory_.make(),
        .capacity_wait = std::nullopt,
        .peer_half_closed = false,
        .half_close_sent = false,
        .done_reading = false,
    };

    SessionEndpoint session_endpoint = {
        .fd = std::move(fd),
        .state = std::move(endpoint_state),
    };

    return session_endpoint;
}

SessionManager::ManagedSession& SessionManager::getManagedSession(SessionId id) {
    auto it = sessions_.find(id);
    assert(it != sessions_.end() && "session ID not found");
    return it->second;
}

const SessionManager::ManagedSession& SessionManager::getManagedSession(SessionId id) const {
    auto it = sessions_.find(id);
    assert(it != sessions_.end() && "session ID not found");
    return it->second;
}

Session& SessionManager::getSession(SessionId id) { return getManagedSession(id).session; }

const Session& SessionManager::getSession(SessionId id) const {
    return getManagedSession(id).session;
}

SessionEndpoint& SessionManager::getEndpoint(Session& session, EndpointRole role) {
    return role == EndpointRole::Downstream ? session.downstream : session.upstream;
}

const SessionEndpoint& SessionManager::getEndpoint(const Session& session, EndpointRole role) {
    return role == EndpointRole::Downstream ? session.downstream : session.upstream;
}

SessionEndpoint& SessionManager::getOtherEndpoint(Session& session, EndpointRole role) {
    return role == EndpointRole::Downstream ? session.upstream : session.downstream;
}

const SessionEndpoint& SessionManager::getOtherEndpoint(const Session& session, EndpointRole role) {
    return role == EndpointRole::Downstream ? session.upstream : session.downstream;
}

EndpointInterests SessionManager::getInterests(const SessionEndpoint& endpoint,
                                               const SessionEndpoint& other) {
    return EndpointInterests{
        // Stop reading once the destination has hung up: anything we read could only be forwarded
        // to a socket that can no longer receive it.
        .readable = !endpoint.state.done_reading && other.state.canWrite() &&
                    !endpoint.state.capacity_wait &&
                    other.state.send_buffer->status() == SendBuffer::BufferStatus::Accepting,
        // Never attempt to write to a hung-up socket.
        .writable = endpoint.state.canWrite() && !endpoint.state.send_buffer->empty(),
        .peer_half_close = !endpoint.state.peer_half_closed && !endpoint.state.done_reading,
        .edge_triggered = endpoint.state.hungUp(),
    };
}

} // namespace orbit::proxy::detail
