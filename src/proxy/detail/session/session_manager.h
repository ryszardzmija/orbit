#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "config/resources.h"
#include "net/socket_io.h"
#include "proxy/detail/buffer/send_buffer_factory.h"
#include "proxy/detail/buffer/send_buffer_options.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/session/idle_deadline.h"
#include "proxy/detail/session/session_id.h"
#include "proxy/detail/session/session_registration.h"
#include "proxy/detail/session/session_state.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

struct SessionManagerOptions {
    SendBufferOptions send_buffer;
    size_t forwarder_buffer_capacity;
    size_t sender_buffer_capacity;
    size_t buffer_budget = ResourceConfig::default_buffer_budget;
};

struct SessionSourceIds {
    SourceId downstream;
    SourceId upstream;
};

struct EndpointInterests {
    bool readable;
    bool writable;
    bool peer_half_close;
    bool edge_triggered;
};

struct SessionInterests {
    EndpointInterests downstream;
    EndpointInterests upstream;
};

enum class SessionEventAction {
    KeepOpen,
    Close,
};

enum class SessionRetryKind { ResidualRead, FinishWrite };

struct SessionRetry {
    EndpointRole role;
    SessionRetryKind kind;
};

struct SessionEventResult {
    SessionEventAction action;
    std::optional<std::string> error;
    std::optional<SessionRetry> retry = std::nullopt;
};

// Owns active sessions and performs session-local I/O and state transitions.
class SessionManager {
public:
    explicit SessionManager(const SessionManagerOptions& options,
                            net::StreamOperations operations = {},
                            AllocationChecks allocation = {});
    SessionManager(SessionManager&&) = default;
    SessionManager& operator=(SessionManager&&) = delete;

    SessionId allocateId();
    std::optional<Session> prepare(FileDescriptor downstream, FileDescriptor upstream) const;
    void add(SessionId id, Session session, SessionSourceIds source_ids);
    void remove(SessionId id);

    bool contains(SessionId id) const;
    bool empty() const;
    size_t size() const { return sessions_.size(); }
    auto ids() const { return std::views::keys(sessions_); }
    SessionSourceIds sourceIds(SessionId id) const;
    SessionInterests interests(SessionId id) const;
    const BufferBudget& bufferBudget() const { return *buffer_budget_; }
    size_t waitingReadCount() const { return waiting_reads_.size(); }
    bool hasReadyRead() const;
    std::optional<SourceId> takeReadyRead();

    // The session retains ownership after its source registration is retired.
    bool shouldRetire(SessionId id, EndpointRole role) const;

    SessionEventResult handleReadable(SessionId id, EndpointRole role, IdleDeadline* idle = nullptr,
                                      bool from_wait_queue = false);
    SessionEventResult handleWritable(SessionId id, EndpointRole role,
                                      IdleDeadline* idle = nullptr);
    SessionEventResult handlePeerHalfClosed(SessionId id, EndpointRole role,
                                            IdleDeadline* idle = nullptr);
    SessionEventResult handleHangup(SessionId id, EndpointRole role, IdleDeadline* idle = nullptr);
    SessionEventResult handleError(SessionId id, EndpointRole role);
    SessionEventResult retryInterrupted(SessionId id, SessionRetry retry, IdleDeadline* idle);

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
    SessionEventResult finishWrite(Session& session, EndpointRole destination_role,
                                   IdleDeadline* idle);
    void cancelReadWait(SessionEndpoint& endpoint);

    // The budget stays at a stable address and outlives its borrowers.
    std::unique_ptr<BufferBudget> buffer_budget_;
    absl::flat_hash_map<SessionId, ManagedSession> sessions_;
    WaitingReads waiting_reads_;
    size_t block_size_;
    AllocationChecks allocation_;
    SessionIdGenerator session_id_generator_;
    SendBufferFactory send_buffer_factory_;
    Forwarder forwarder_;
    PendingDataSender sender_;
    net::StreamOperations operations_;
};

} // namespace orbit::proxy::detail
