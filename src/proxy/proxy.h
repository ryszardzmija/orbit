#pragma once

#include <expected>
#include <system_error>

#include <absl/container/flat_hash_map.h>

#include "common/fd.h"
#include "proxy/detail/forwarding.h"
#include "proxy/detail/pending_data_sender.h"
#include "proxy/detail/reactor_generator.h"
#include "proxy/detail/session_pair.h"

namespace orbit::proxy {

class ProxyReactor {
public:
    static std::expected<ProxyReactor, std::error_code> create(FileDescriptor downstream_fd,
                                                               FileDescriptor upstream_fd);

    [[nodiscard]] std::expected<void, std::error_code> start();

private:
    constexpr static size_t block_size = 4096;
    constexpr static size_t high_watermark = block_size * 64;
    constexpr static size_t low_watermark = block_size * 48;

    constexpr static size_t event_buf_cap = 64;
    constexpr static size_t forwarder_buf_cap = 4096;
    constexpr static size_t sender_buf_cap = 4096;

    constexpr static int graceful_shutdown_timeout_s = 30;

    enum class ShutdownState {
        Running,
        GracefullyStopping,
        HardStopping,
    };

    ProxyReactor(FileDescriptor epfd, FileDescriptor shutdown_signal_fd,
                 FileDescriptor shutdown_timer_fd);

    std::expected<void, std::error_code> registerEndpoint(int fd, uint32_t initial_events,
                                                          detail::SessionId session_id,
                                                          detail::EndpointRole role,
                                                          detail::ReactorSourceId id);
    std::expected<void, std::error_code> registerListener(int fd,
                                                          detail::ReactorSourceId source_id);
    std::expected<void, std::error_code> registerShutdownSignalEvent();
    std::expected<void, std::error_code> registerShutdownTimerEvent();
    std::expected<void, std::error_code>
    registerReactorSource(int fd, uint32_t initial_events, detail::ReactorSourceId id,
                          const detail::ReactorRegistration& reactor_registration);
    std::expected<void, std::error_code> addSession(FileDescriptor upstream_fd,
                                                    FileDescriptor downstream_fd);
    std::expected<void, std::error_code> closeSession(detail::SessionId session_id);
    std::expected<void, std::error_code> handleEndpoint(detail::EndpointRegistration registration,
                                                        uint32_t event_mask);
    std::expected<void, std::error_code> handleShutdownSignal(detail::ShutdownSignalRegistration);
    std::expected<void, std::error_code> handleShutdownTimer(detail::ShutdownTimerRegistration);
    std::expected<void, std::error_code> handleListener(detail::ListenerRegistration);
    void closeSessionAndLog(detail::SessionId session_id);
    std::expected<void, std::error_code> forceCloseSession(detail::SessionId session_id);
    std::expected<void, std::error_code> forceCloseAllSessions();
    void performHardStop();
    detail::SessionEndpoint& getEndpoint(detail::ManagedSession& managed_session,
                                         detail::EndpointRole role);
    detail::ReactorSourceId getEndpointId(detail::ManagedSession& managed_session,
                                          detail::EndpointRole role);
    detail::ReactorSourceId getOtherEndpointId(detail::ManagedSession& managed_session,
                                               detail::EndpointRole role);
    bool hasActiveSessions() const;
    std::expected<void, std::error_code> handleShutdownRequest();
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
    ShutdownState shutdown_state_ = ShutdownState::Running;
};

} // namespace orbit::proxy
