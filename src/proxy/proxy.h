#pragma once

#include <list>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "common/steady_time_source.h"
#include "config/resources.h"
#include "config/timeouts.h"
#include "net/listener.h"
#include "net/socket_io.h"
#include "proxy/detail/active_listener.h"
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

enum class ConnectionOperation {
    AllocateBuffers,
    AllocateDeadline,
    AllocateDeferredDial,
    AddWatch,
    ChangeWatch,
    RemoveWatch,
    EstablishmentDeadline,
};

struct ConnectionFailure {
    ConnectionOperation operation;
    std::error_code code;
    std::error_code rollback_error = {};
};

struct ReactorOperations {
    net::StreamOperations stream;
    net::AcceptOperations accept;
    detail::EpollOperations epoll;
    detail::AllocationChecks allocation;
};

class ProxyReactor {
public:
    static Result<ProxyReactor, ProxyCreateError>
    create(const net::ListenSocketAddress& listen_address,
           const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts = {},
           ResourceConfig resources = {});

    [[nodiscard]] Status<ProxyRuntimeError> start();

private:
    friend class ProxyReactorTest;

    constexpr static size_t forwarder_buf_cap = 4096;
    constexpr static size_t sender_buf_cap = 4096;

    constexpr static std::chrono::seconds graceful_shutdown_timeout{30};
    constexpr static int max_expirations_per_dispatch = 64;
    constexpr static int max_backlog_size = 64;
    constexpr static int max_accept_batch_size = 16;
    constexpr static size_t max_dial_attempts_per_batch = 8;
    constexpr static size_t max_resumed_dials_per_turn = 8;
    constexpr static std::chrono::milliseconds admission_retry_interval{100};
    constexpr static std::chrono::milliseconds interrupted_io_retry_interval{1};

    enum class ShutdownState {
        Running,
        GracefullyStopping,
        HardStopping,
    };

    enum class EndpointEventOutcome {
        KeepSession,
        SessionClosed,
    };

    class AdmissionReservation {
    public:
        explicit AdmissionReservation(bool& transient);
        AdmissionReservation(AdmissionReservation&& other) noexcept;
        AdmissionReservation& operator=(AdmissionReservation&&) = delete;
        ~AdmissionReservation();

        void release() noexcept;

    private:
        bool* transient_;
    };

    struct ExtractedPending {
        // Destruction closes the sockets before releasing their admission.
        AdmissionReservation admission;
        detail::PendingConnection connection;
    };

    using DeferredDials = std::list<detail::SourceId>;

    enum class DeferredDialKind { SocketAttempt, CompletionQuery };

    struct QueuedDial {
        DeferredDialKind kind;
        DeferredDials::iterator position;
    };

    struct PendingEntry {
        detail::PendingConnection connection;
        std::optional<QueuedDial> deferred;
        bool watched;
    };

    ProxyReactor(detail::EpollPoller poller, FileDescriptor shutdown_signal_fd,
                 FileDescriptor timer_fd, detail::UpstreamDialer upstream_dialer,
                 TimeoutConfig timeouts, ResourceConfig resources, SteadyTimeSource time_source,
                 ReactorOperations operations);

    static Result<ProxyReactor, ProxyCreateError>
    createImpl(const net::ListenSocketAddress& listen_address,
               const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts,
               ResourceConfig resources, SteadyTimeSource time_source,
               ReactorOperations operations);

    // Resource registration
    Result<detail::SourceId, std::error_code> registerEndpoint(int fd, uint32_t initial_events,
                                                               detail::SessionId session_id,
                                                               detail::EndpointRole role);
    Result<detail::SourceId, std::error_code> registerListener(int fd);
    Result<detail::SourceId, std::error_code> registerShutdownSignalEvent();
    Result<detail::SourceId, std::error_code> registerTimerEvent();
    Result<detail::SourceId, ConnectionFailure>
    registerPendingConnection(detail::PendingConnection pending_connection,
                              AdmissionReservation& admission, bool interrupted = false);

    Result<detail::SourceId, std::error_code>
    registerReactorSource(int fd, uint32_t initial_events,
                          const detail::ReactorRegistration& reactor_registration);
    Status<std::error_code> unregisterReactorSource(detail::SourceId id);

    // Session lifecycle
    Result<detail::SessionId, ConnectionFailure> addSession(FileDescriptor upstream_fd,
                                                            FileDescriptor downstream_fd,
                                                            detail::Deadline deadline,
                                                            AdmissionReservation& admission);
    Status<std::error_code> closeSession(detail::SessionId session_id);

    Result<ExtractedPending, std::error_code> takePending(detail::SourceId id);
    Status<std::error_code> cancelPendingConnections();

    // Event handlers
    Status<FatalReactorError> runTurn();
    Status<FatalReactorError> dispatchBatch(std::span<const detail::ReadyEvent> events);
    Status<FatalReactorError> dispatch(detail::ReadyEvent event, size_t& dial_attempts_left,
                                       bool from_wait_queue = false);
    Status<FatalReactorError> handleEndpoint(detail::EndpointRegistration registration,
                                             uint32_t event_mask, bool from_wait_queue,
                                             std::optional<detail::SessionRetryKind> retry = {});
    Status<FatalReactorError> resumeReadyRead();
    Status<FatalReactorError> handleShutdownSignal(detail::ShutdownSignalRegistration);
    Status<FatalReactorError> handleTimer(detail::TimerRegistration);
    Status<FatalReactorError> handleListener(detail::ListenerRegistration, size_t& attempts_left);
    Status<FatalReactorError> handlePendingConnection(detail::SourceId id, size_t& attempts_left);

    // Handlers for handlePendingConnection()
    Status<FatalReactorError> handleDialResult(detail::UpstreamDialResult dial_result,
                                               AdmissionReservation& admission);
    Status<FatalReactorError> handleDialError(net::DialError dial_error);
    Status<FatalReactorError> handleConnectionFailure(ConnectionFailure failure);
    Status<FatalReactorError> resumeDeferredDials();
    bool hasRunnableDials() const;
    DeferredDials& deferredQueue(DeferredDialKind kind);
    QueuedDial enqueueDeferredDial(detail::SourceId id, const detail::PendingConnection& pending);

    Status<std::error_code> handleShutdownRequest();
    Status<std::error_code> synchronizeTimer();
    Status<FatalReactorError> expireDeadlines(detail::Deadline now);
    Status<std::error_code> synchronizeListener();
    std::optional<detail::Deadline> nextWakeup() const;
    void suspendAdmission(const char* operation, std::error_code code, bool report = true);
    size_t pendingConnectionCount() const;
    bool canAdmit() const;

    // Session state synchronization
    Result<EndpointEventOutcome, FatalReactorError>
    applySessionEventResult(detail::SessionId session_id,
                            const detail::SessionEventResult& event_result,
                            detail::IdleDeadline* idle);
    Status<ConnectionFailure> synchronizeSessionInterests(detail::SessionId session_id);
    Status<ConnectionFailure> synchronizeEndpoint(detail::SessionId session_id,
                                                  detail::EndpointRole role,
                                                  detail::SourceId source_id,
                                                  const detail::EndpointInterests& interests);

    // Reactor shutdown
    Status<std::error_code> forceCloseAllSessions();

    // Reactor state tracking
    bool hasActiveSessions() const;
    bool shouldStop() const;

    detail::EpollPoller poller_;
    detail::ReactorSources sources_;
    detail::SessionManager session_manager_;
    FileDescriptor shutdown_signal_fd_;
    FileDescriptor timer_fd_;
    detail::Deadlines deadlines_;
    std::optional<detail::Deadline> armed_deadline_;
    std::optional<detail::Deadline> admission_retry_at_;
    bool admission_pause_reported_ = false;
    TimeoutConfig timeouts_;
    size_t max_connections_;
    size_t max_pending_connections_;
    // The reactor has at most one admission between ownership records at any instant.
    bool transient_admission_ = false;
    SteadyTimeSource time_source_;
    net::AcceptOperations accept_operations_;
    detail::AllocationChecks allocation_;
    DeferredDials deferred_attempts_;
    DeferredDials deferred_completions_;
    absl::flat_hash_map<detail::SourceId, PendingEntry> pending_connections_;
    std::optional<detail::ActiveListener> active_listener_;
    detail::UpstreamDialer upstream_dialer_;
    ShutdownState shutdown_state_ = ShutdownState::Running;
};

} // namespace orbit::proxy
