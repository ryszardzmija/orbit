#pragma once

#include <expected>
#include <string>
#include <system_error>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "net/dialer.h"
#include "net/listener.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/reactor_generator.h"

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
    static std::expected<ProxyReactor, ProxyCreateError>
    create(const net::ListenSocketAddress& listen_address,
           const net::DialSocketAddress& dial_address);

    [[nodiscard]] std::expected<void, ProxyRuntimeError> start();

private:
    constexpr static size_t block_size = 4096;
    constexpr static size_t high_watermark = block_size * 64;
    constexpr static size_t low_watermark = block_size * 48;

    constexpr static size_t event_buf_cap = 64;
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

    ProxyReactor(FileDescriptor epfd, FileDescriptor shutdown_signal_fd,
                 FileDescriptor shutdown_timer_fd, detail::Forwarder forwarder,
                 detail::PendingDataSender sender, net::Listener listener,
                 const net::DialSocketAddress& dial_address);

    // Resource registration
    std::expected<void, std::error_code> registerEndpoint(int fd, uint32_t initial_events,
                                                          detail::SessionId session_id,
                                                          detail::EndpointRole role,
                                                          detail::ReactorSourceId id);
    std::expected<void, std::error_code> registerListener();
    std::expected<void, std::error_code> registerShutdownSignalEvent();
    std::expected<void, std::error_code> registerShutdownTimerEvent();
    std::expected<void, std::error_code>
    registerReactorSource(int fd, uint32_t initial_events, detail::ReactorSourceId id,
                          const detail::ReactorRegistration& reactor_registration);
    std::expected<void, std::error_code> unregisterReactorSource(int fd,
                                                                 detail::ReactorSourceId id);

    // Session lifecycle
    std::expected<void, AddSessionError> addSession(FileDescriptor upstream_fd,
                                                    FileDescriptor downstream_fd);
    std::expected<void, std::error_code> closeSession(detail::SessionId session_id);
    void closeSessionAndLog(detail::SessionId session_id);

    // Event handlers
    std::expected<void, FatalReactorError> handleEndpoint(detail::EndpointRegistration registration,
                                                          uint32_t event_mask);
    std::expected<void, FatalReactorError> handleShutdownSignal(detail::ShutdownSignalRegistration);
    std::expected<void, FatalReactorError> handleShutdownTimer(detail::ShutdownTimerRegistration);
    std::expected<void, FatalReactorError> handleListener(detail::ListenerRegistration);

    std::expected<EndpointEventOutcome, FatalReactorError>
    handleEndpointReadable(detail::SessionId session_id, detail::ReactorSourceId source_id,
                           detail::SessionEndpoint& source, detail::ReactorSourceId destination_id,
                           detail::SessionEndpoint& destination);
    std::expected<EndpointEventOutcome, FatalReactorError>
    handleEndpointWritable(detail::SessionId session_id, detail::ReactorSourceId source_id,
                           detail::SessionEndpoint& source, detail::ReactorSourceId destination_id,
                           detail::SessionEndpoint& destination);
    std::expected<EndpointEventOutcome, FatalReactorError>
    handleEndpointPeerHalfClosed(detail::SessionId session_id, detail::ReactorSourceId endpoint_id,
                                 detail::SessionEndpoint& endpoint,
                                 detail::ReactorSourceId other_endpoint_id,
                                 detail::SessionEndpoint& other_endpoint);
    std::expected<EndpointEventOutcome, FatalReactorError>
    handleEndpointHangup(detail::SessionId session_id);
    std::expected<EndpointEventOutcome, FatalReactorError>
    handleEndpointError(detail::SessionId session_id, detail::SessionEndpoint& endpoint);

    std::expected<void, std::error_code> handleShutdownRequest();

    // epoll interest list synchronization
    std::expected<void, std::error_code> ensureReadableInterest(detail::ReactorSourceId source_id,
                                                                detail::SessionEndpoint& endpoint);
    std::expected<void, std::error_code>
    ensureNoReadableInterest(detail::ReactorSourceId source_id, detail::SessionEndpoint& endpoint);
    std::expected<void, std::error_code> ensureWritableInterest(detail::ReactorSourceId source_id,
                                                                detail::SessionEndpoint& endpoint);
    std::expected<void, std::error_code>
    ensureNoWritableInterest(detail::ReactorSourceId source_id, detail::SessionEndpoint& endpoint);
    std::expected<void, std::error_code>
    disablePeerHalfCloseEvents(detail::ReactorSourceId source_id,
                               detail::SessionEndpoint& endpoint);

    // Reactor shutdown
    std::expected<void, std::error_code> forceCloseSession(detail::SessionId session_id);
    std::expected<void, std::error_code> forceCloseAllSessions();
    void performHardStop();

    // Reactor state tracking
    bool hasActiveSessions() const;
    bool shouldStop() const;

    FileDescriptor epfd_;
    FileDescriptor shutdown_signal_fd_;
    FileDescriptor shutdown_timer_fd_;
    absl::flat_hash_map<detail::SessionId, detail::ManagedSession> sessions_;
    absl::flat_hash_map<detail::ReactorSourceId, detail::ReactorRegistration> registrations_;
    detail::SessionIdGenerator session_id_generator_;
    detail::ReactorSourceIdGenerator reactor_source_id_generator_;
    detail::SendBufferFactory send_buffer_factory_;
    detail::Forwarder forwarder_;
    detail::PendingDataSender sender_;
    net::Listener listener_;
    ShutdownState shutdown_state_ = ShutdownState::Running;
    net::DialSocketAddress dial_address;
};

} // namespace orbit::proxy
