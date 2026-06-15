#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "proxy/detail/buffer/send_buffer_factory.h"
#include "proxy/detail/buffer/send_buffer_options.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/session/session_id.h"
#include "proxy/detail/session/session_registration.h"
#include "proxy/detail/session/session_state.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

struct SessionManagerOptions {
    SendBufferOptions send_buffer;
    size_t forwarder_buffer_capacity;
    size_t sender_buffer_capacity;
};

struct SessionSourceIds {
    SourceId downstream;
    SourceId upstream;
};

struct EndpointInterests {
    bool readable;
    bool writable;
    bool peer_half_close;
};

struct SessionInterests {
    EndpointInterests downstream;
    EndpointInterests upstream;
};

enum class SessionEventAction {
    KeepOpen,
    Close,
};

struct SessionEventResult {
    SessionEventAction action;
    std::optional<std::string> error;
};

// Owns active sessions and performs session-local I/O and state transitions.
class SessionManager {
public:
    explicit SessionManager(const SessionManagerOptions& options);

    SessionId allocateId();
    void add(SessionId id, FileDescriptor downstream, FileDescriptor upstream,
             SessionSourceIds source_ids);
    void remove(SessionId id);

    bool contains(SessionId id) const;
    bool empty() const;
    SessionId firstId() const;
    SessionSourceIds sourceIds(SessionId id) const;
    SessionInterests interests(SessionId id) const;

    // Whether an endpoint should be retired from the poller: it has hung up and can therefore
    // produce no further useful events. Leaving it registered would spin the reactor because
    // EPOLLHUP is reported unconditionally.
    bool shouldRetire(SessionId id, EndpointRole role) const;

    SessionEventResult handleReadable(SessionId id, EndpointRole role);
    SessionEventResult handleWritable(SessionId id, EndpointRole role);
    SessionEventResult handlePeerHalfClosed(SessionId id, EndpointRole role);
    SessionEventResult handleHangup(SessionId id, EndpointRole role);
    SessionEventResult handleError(SessionId id, EndpointRole role);

private:
    struct ManagedSession {
        Session session;
        SessionSourceIds source_ids;
    };

    SessionEndpoint getInitialSessionEndpoint(FileDescriptor fd) const;
    ManagedSession& getManagedSession(SessionId id);
    const ManagedSession& getManagedSession(SessionId id) const;
    Session& getSession(SessionId id);
    const Session& getSession(SessionId id) const;
    static SessionEndpoint& getEndpoint(Session& session, EndpointRole role);
    static const SessionEndpoint& getEndpoint(const Session& session, EndpointRole role);
    static SessionEndpoint& getOtherEndpoint(Session& session, EndpointRole role);
    static const SessionEndpoint& getOtherEndpoint(const Session& session, EndpointRole role);
    static EndpointInterests getInterests(const SessionEndpoint& endpoint,
                                          const SessionEndpoint& other);

    absl::flat_hash_map<SessionId, ManagedSession> sessions_;
    SessionIdGenerator session_id_generator_;
    SendBufferFactory send_buffer_factory_;
    Forwarder forwarder_;
    PendingDataSender sender_;
};

} // namespace orbit::proxy::detail
