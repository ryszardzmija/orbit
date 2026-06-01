#pragma once

#include <optional>
#include <string>
#include <system_error>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "net/listener.h"
#include "proxy/detail/active_listener.h"
#include "proxy/detail/dial/connection_id.h"
#include "proxy/detail/epoll/epoll_poller.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_connection.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/session/session_id.h"
#include "proxy/detail/session/session_types.h"
#include "proxy/detail/sources/reactor_sources.h"
#include "proxy/detail/upstream_dialer.h"

namespace orbit::proxy {

// TODO: Document the meaning of these errors
struct ProxyCreateError {
    std::string message;
};

struct ProxyRuntimeError {
    std::string message;
};

struct FatalReactorError {
    std::string message;
};

enum class RollbackStatus {
    Success,
    Failure,
};

struct AddSessionError {
    std::string message;
    RollbackStatus status;
};

enum class EndpointEventOutcome {
    KeepSession,
    SessionClosed,
};

class ProxyReactor {
public:
    static Result<ProxyReactor, ProxyCreateError>
    create(const net::ListenSocketAddress& listen_address,
           const net::ResolutionEndpoint& upstream_address);

    [[nodiscard]] Status<ProxyRuntimeError> start();

private:
    constexpr static size_t block_size = 4096;
    constexpr static size_t high_watermark = block_size * 64;
    constexpr static size_t low_watermark = block_size * 48;

    constexpr static size_t forwarder_buf_cap = 4096;
    constexpr static size_t sender_buf_cap = 4096;

    constexpr static int graceful_shutdown_timeout_s = 30;
    constexpr static int max_backlog_size = 64;
    constexpr static int max_accept_batch_size = 16;

    enum class ShutdownState {
        Running,
        GracefullyStopping,
        HardStopping,
    };

    ProxyReactor(detail::EpollPoller poller, FileDescriptor shutdown_signal_fd,
                 FileDescriptor shutdown_timer_fd, detail::Forwarder forwarder,
                 detail::PendingDataSender sender, detail::UpstreamDialer upstream_dialer);

    // Resource registration
    Result<detail::SourceId, std::error_code> registerEndpoint(int fd, uint32_t initial_events,
                                                               detail::SessionId session_id,
                                                               detail::EndpointRole role);
    Result<detail::SourceId, std::error_code> registerListener(int fd);
    Result<detail::SourceId, std::error_code> registerShutdownSignalEvent();
    Result<detail::SourceId, std::error_code> registerShutdownTimerEvent();
    Result<detail::SourceId, std::error_code>
    registerPendingConnection(detail::PendingConnection pending_connection);

    Result<detail::SourceId, std::error_code>
    registerReactorSource(int fd, uint32_t initial_events,
                          const detail::ReactorRegistration& reactor_registration);
    Status<std::error_code> unregisterReactorSource(detail::SourceId id);

    // Session lifecycle
    Status<AddSessionError> addSession(FileDescriptor upstream_fd, FileDescriptor downstream_fd);
    Status<std::error_code> closeSession(detail::SessionId session_id);
    void closeSessionAndLog(detail::SessionId session_id);

    // Event handlers
    Status<FatalReactorError> handleEndpoint(detail::EndpointRegistration registration,
                                             uint32_t event_mask);
    Status<FatalReactorError> handleShutdownSignal(detail::ShutdownSignalRegistration);
    Status<FatalReactorError> handleShutdownTimer(detail::ShutdownTimerRegistration);
    Status<FatalReactorError> handleListener(detail::ListenerRegistration);
    Status<FatalReactorError> handlePendingConnection(detail::SourceId id,
                                                      detail::PendingDialRegistration registration);

    // Handlers for handleEndpoint()
    Result<EndpointEventOutcome, FatalReactorError>
    handleEndpointReadable(detail::SessionId session_id, detail::SourceId source_id,
                           detail::SessionEndpoint& source, detail::SourceId destination_id,
                           detail::SessionEndpoint& destination);
    Result<EndpointEventOutcome, FatalReactorError>
    handleEndpointWritable(detail::SessionId session_id, detail::SourceId source_id,
                           detail::SessionEndpoint& source, detail::SourceId destination_id,
                           detail::SessionEndpoint& destination);
    Result<EndpointEventOutcome, FatalReactorError>
    handleEndpointPeerHalfClosed(detail::SessionId session_id, detail::SourceId endpoint_id,
                                 detail::SessionEndpoint& endpoint);
    Result<EndpointEventOutcome, FatalReactorError>
    handleEndpointHangup(detail::SessionId session_id);
    Result<EndpointEventOutcome, FatalReactorError>
    handleEndpointError(detail::SessionId session_id, detail::SessionEndpoint& endpoint);

    // Handlers for handlePendingConnection()
    Status<FatalReactorError> handleDialResult(detail::UpstreamDialResult dial_result);
    Status<FatalReactorError> handleDialError(detail::UpstreamDialError dial_error);

    Status<std::error_code> handleShutdownRequest();

    // epoll interest list synchronization
    Status<std::error_code> ensureReadableInterest(detail::SourceId source_id);
    Status<std::error_code> ensureNoReadableInterest(detail::SourceId source_id);
    Status<std::error_code> ensureWritableInterest(detail::SourceId source_id);
    Status<std::error_code> ensureNoWritableInterest(detail::SourceId source_id);
    Status<std::error_code> disablePeerHalfCloseEvents(detail::SourceId source_id);

    // Reactor shutdown
    Status<std::error_code> forceCloseSession(detail::SessionId session_id);
    Status<std::error_code> forceCloseAllSessions();
    void performHardStop();

    // Reactor state tracking
    bool hasActiveSessions() const;
    bool shouldStop() const;

    detail::EpollPoller poller_;
    detail::ReactorSources sources_;
    FileDescriptor shutdown_signal_fd_;
    FileDescriptor shutdown_timer_fd_;
    absl::flat_hash_map<detail::SessionId, detail::ManagedSession> sessions_;
    absl::flat_hash_map<detail::PendingConnectionId, detail::PendingConnection>
        pending_connections_;
    detail::SessionIdGenerator session_id_generator_;
    detail::PendingConnectionIdGenerator pending_connection_id_generator_;
    detail::SendBufferFactory send_buffer_factory_;
    detail::Forwarder forwarder_;
    detail::PendingDataSender sender_;
    std::optional<detail::ActiveListener> active_listener_;
    detail::UpstreamDialer upstream_dialer_;
    ShutdownState shutdown_state_ = ShutdownState::Running;
};

} // namespace orbit::proxy
