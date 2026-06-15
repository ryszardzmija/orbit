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

bool shouldClose(const Session& session) {
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
        .readable = !endpoint.state.done_reading &&
                    other.state.send_buffer->status() == SendBuffer::BufferStatus::Accepting,
        .writable = !endpoint.state.send_buffer->empty(),
        .peer_half_close = !endpoint.state.peer_half_closed,
    };
}

} // namespace orbit::proxy::detail
