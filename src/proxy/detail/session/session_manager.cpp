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

Status<std::error_code> halfCloseConnection(int socket_fd) {
    if (int result = shutdown(socket_fd, SHUT_WR); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

bool shouldHalfClose(const SessionEndpoint& source, const SessionEndpoint& destination) {
    return source.state.peer_half_closed && source.state.done_reading &&
           destination.state.send_buffer->empty();
}

Status<std::error_code> halfCloseIfReady(const SessionEndpoint& source,
                                         SessionEndpoint& destination) {
    if (!shouldHalfClose(source, destination) || destination.state.half_close_sent) {
        return {};
    }

    if (auto result = halfCloseConnection(destination.fd.get()); !result) {
        return result;
    }
    destination.state.half_close_sent = true;

    return {};
}

bool isHungUp(const Session& session) {
    return session.downstream.state.write_closed || session.upstream.state.write_closed;
}

// Whether the source->destination stream has nothing left to deliver: either the destination has
// hung up (its buffered data is undeliverable), or we have read everything from the source and
// flushed it all to the destination.
bool directionComplete(const SessionEndpoint& source, const SessionEndpoint& destination) {
    return destination.state.write_closed ||
           (source.state.done_reading && destination.state.send_buffer->empty());
}

bool shouldClose(const Session& session) {
    // Once either side has hung up the half-close handshake can no longer complete on its own (a
    // half-close cannot be propagated through a socket we can no longer write to), so the session
    // is torn down as soon as both directions have delivered everything they still can.
    if (isHungUp(session)) {
        return directionComplete(session.downstream, session.upstream) &&
               directionComplete(session.upstream, session.downstream);
    }

    return session.downstream.state.half_close_sent && session.upstream.state.half_close_sent;
}

SessionEventResult keepOpen(std::optional<std::string> error = std::nullopt) {
    return SessionEventResult{
        .action = SessionEventAction::KeepOpen,
        .error = std::move(error),
    };
}

SessionEventResult closeSession(std::optional<std::string> error = std::nullopt) {
    return SessionEventResult{
        .action = SessionEventAction::Close,
        .error = std::move(error),
    };
}

} // namespace

SessionManager::SessionManager(const SessionManagerOptions& options)
    : send_buffer_factory_(options.send_buffer),
      forwarder_(Forwarder::create(options.forwarder_buffer_capacity)),
      sender_(PendingDataSender::create(options.sender_buffer_capacity)) {}

SessionId SessionManager::allocateId() { return session_id_generator_.getNextId(); }

void SessionManager::add(SessionId id, FileDescriptor downstream, FileDescriptor upstream,
                         SessionSourceIds source_ids) {
    ManagedSession managed_session = {
        .session =
            {
                .downstream = getInitialSessionEndpoint(std::move(downstream)),
                .upstream = getInitialSessionEndpoint(std::move(upstream)),
            },
        .source_ids = source_ids,
    };

    auto [it, inserted] = sessions_.emplace(id, std::move(managed_session));
    assert(inserted && "session ID already added");
}

void SessionManager::remove(SessionId id) {
    size_t erased = sessions_.erase(id);
    assert(erased == 1 && "session ID not found");
}

bool SessionManager::contains(SessionId id) const { return sessions_.contains(id); }

bool SessionManager::empty() const { return sessions_.empty(); }

SessionId SessionManager::firstId() const {
    assert(!sessions_.empty() && "no sessions are managed");
    return sessions_.begin()->first;
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
    // handleHangup() always marks the endpoint done_reading once it sets write_closed, so a
    // write-closed endpoint can neither be written to nor read from again.
    return getEndpoint(getSession(id), role).state.write_closed;
}

SessionEventResult SessionManager::handleReadable(SessionId id, EndpointRole role) {
    Session& session = getSession(id);
    SessionEndpoint& source = getEndpoint(session, role);
    SessionEndpoint& destination = getOtherEndpoint(session, role);

    if (auto result = forwarder_.forward(source, destination); !result) {
        return closeSession("Forwarding failed: " + result.error().message);
    }

    if (auto result = halfCloseIfReady(source, destination); !result) {
        return keepOpen("Half-closing connection failed: " + result.error().message());
    }

    return shouldClose(session) ? closeSession() : keepOpen();
}

SessionEventResult SessionManager::handleWritable(SessionId id, EndpointRole role) {
    Session& session = getSession(id);
    SessionEndpoint& destination = getEndpoint(session, role);
    SessionEndpoint& source = getOtherEndpoint(session, role);

    if (auto result = sender_.sendPending(destination); !result) {
        return closeSession("Sending pending outbound data failed: " + result.error().message);
    }

    if (auto result = halfCloseIfReady(source, destination); !result) {
        return keepOpen("Half-closing connection failed: " + result.error().message());
    }

    return shouldClose(session) ? closeSession() : keepOpen();
}

SessionEventResult SessionManager::handlePeerHalfClosed(SessionId id, EndpointRole role) {
    Session& session = getSession(id);
    SessionEndpoint& source = getEndpoint(session, role);
    SessionEndpoint& destination = getOtherEndpoint(session, role);

    source.state.peer_half_closed = true;

    if (auto result = halfCloseIfReady(source, destination); !result) {
        return keepOpen("Half-closing connection failed: " + result.error().message());
    }

    return shouldClose(session) ? closeSession() : keepOpen();
}

SessionEventResult SessionManager::handleHangup(SessionId id, EndpointRole role) {
    Session& session = getSession(id);
    SessionEndpoint& hungup = getEndpoint(session, role);
    SessionEndpoint& other = getOtherEndpoint(session, role);

    hungup.state.write_closed = true;
    // The connection is fully torn down, so the peer will not send anything more either.
    hungup.state.peer_half_closed = true;

    // Forward whatever the hung-up peer already sent us so it still reaches the other endpoint,
    // unless that endpoint has also hung up and can no longer receive it.
    if (!other.state.write_closed) {
        if (auto result = forwarder_.drain(hungup, other); !result) {
            return closeSession("Draining hung-up endpoint failed: " + result.error().message);
        }
    }
    // We will never read from this socket again: the connection is gone, so anything not already
    // drained is lost regardless. Recording this lets the endpoint be retired from the poller.
    hungup.state.done_reading = true;

    return shouldClose(session) ? closeSession() : keepOpen();
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
        .peer_half_closed = false,
        .half_close_sent = false,
        .done_reading = false,
        .write_closed = false,
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
        .readable = !endpoint.state.done_reading && !other.state.write_closed &&
                    other.state.send_buffer->status() == SendBuffer::BufferStatus::Accepting,
        // Never attempt to write to a hung-up socket.
        .writable = !endpoint.state.write_closed && !endpoint.state.send_buffer->empty(),
        .peer_half_close = !endpoint.state.peer_half_closed,
    };
}

} // namespace orbit::proxy::detail
