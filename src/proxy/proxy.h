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
#include "proxy/detail/pending_connection.h"
#include "proxy/detail/session/session_id.h"
#include "proxy/detail/session/session_manager.h"
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

    enum class EndpointEventOutcome {
        KeepSession,
        SessionClosed,
    };

    ProxyReactor(detail::EpollPoller poller, FileDescriptor shutdown_signal_fd,
                 FileDescriptor shutdown_timer_fd, detail::UpstreamDialer upstream_dialer);

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

    // Handlers for handlePendingConnection()
    Status<FatalReactorError> handleDialResult(detail::UpstreamDialResult dial_result);
    Status<FatalReactorError> handleDialError(detail::UpstreamDialError dial_error);

    Status<std::error_code> handleShutdownRequest();

    // Session state synchronization
    Result<EndpointEventOutcome, FatalReactorError>
    applySessionEventResult(detail::SessionId session_id,
                            const detail::SessionEventResult& event_result);
    Status<std::error_code> synchronizeSessionInterests(detail::SessionId session_id);
    Status<std::error_code> synchronizeEndpoint(detail::SessionId session_id,
                                                detail::EndpointRole role, detail::SourceId source_id,
                                                const detail::EndpointInterests& interests);

    // Reactor shutdown
    Status<std::error_code> forceCloseAllSessions();
    void performHardStop();

    // Reactor state tracking
    bool hasActiveSessions() const;
    bool shouldStop() const;

    detail::EpollPoller poller_;
    detail::ReactorSources sources_;
    detail::SessionManager session_manager_;
    FileDescriptor shutdown_signal_fd_;
    FileDescriptor shutdown_timer_fd_;
    absl::flat_hash_map<detail::PendingConnectionId, detail::PendingConnection>
        pending_connections_;
    detail::PendingConnectionIdGenerator pending_connection_id_generator_;
    std::optional<detail::ActiveListener> active_listener_;
    detail::UpstreamDialer upstream_dialer_;
    ShutdownState shutdown_state_ = ShutdownState::Running;
};

} // namespace orbit::proxy
