#include "proxy/proxy.h"

#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

#include <cerrno>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>

#include "common/fd.h"
#include "net/address_format.h"
#include "proxy/detail/epoll/epoll_poller.h"
#include "proxy/detail/session/session_manager.h"
#include "proxy/detail/signal_fd.h"
#include "proxy/detail/timer_fd.h"

namespace orbit::proxy {

namespace {

constexpr detail::DeadlineKey shutdown_deadline{detail::DeadlineKind::Shutdown, 0};

// Lets a set of lambdas be combined into a single callable for std::visit.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

enum class AcceptFailure { Connection, Backoff, Infrastructure };

AcceptFailure classifyAcceptError(const std::error_code& error) {
    switch (error.value()) {
    case EMFILE:
    case ENFILE:
    case ENOMEM:
    case ENOBUFS:
    // A permission check can fail before the protocol dequeues a connection.
    case EPERM:
    case EACCES:
        return AcceptFailure::Backoff;
    case ECONNABORTED:
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
        return AcceptFailure::Connection;
    default:
        return AcceptFailure::Infrastructure;
    }
}

const char* operationName(ConnectionOperation operation) {
    switch (operation) {
    case ConnectionOperation::AllocateBuffers:
        return "allocate send buffers";
    case ConnectionOperation::AllocateDeadline:
        return "allocate connection deadline";
    case ConnectionOperation::AllocateDeferredDial:
        return "allocate deferred dial entry";
    case ConnectionOperation::AddWatch:
        return "epoll ADD";
    case ConnectionOperation::ChangeWatch:
        return "epoll MOD";
    case ConnectionOperation::RemoveWatch:
        return "epoll DEL";
    case ConnectionOperation::EstablishmentDeadline:
        return "connection establishment deadline";
    }
    return "connection setup";
}

uint32_t getEventMask(const detail::EndpointInterests& interests) {
    uint32_t event_mask = interests.edge_triggered ? EPOLLET : 0;
    if (interests.readable) {
        event_mask |= EPOLLIN;
    }
    if (interests.writable) {
        event_mask |= EPOLLOUT;
    }
    if (interests.peer_half_close) {
        event_mask |= EPOLLRDHUP;
    }
    return event_mask;
}

} // namespace

ProxyReactor::AdmissionReservation::AdmissionReservation(bool& transient)
    : transient_(&transient) {
    assert(!transient);
    transient = true;
}

ProxyReactor::AdmissionReservation::AdmissionReservation(AdmissionReservation&& other) noexcept
    : transient_(std::exchange(other.transient_, nullptr)) {}

ProxyReactor::AdmissionReservation::~AdmissionReservation() { release(); }

void ProxyReactor::AdmissionReservation::release() noexcept {
    if (transient_) {
        *transient_ = false;
        transient_ = nullptr;
    }
}

ProxyReactor::ProxyReactor(detail::EpollPoller poller, FileDescriptor shutdown_signal_fd,
                           FileDescriptor timer_fd, detail::UpstreamDialer upstream_dialer,
                           TimeoutConfig timeouts, ResourceConfig resources,
                           SteadyTimeSource time_source, ReactorOperations operations)
    : poller_(std::move(poller)),
      session_manager_(
          detail::SessionManagerOptions{
              .send_buffer =
                  {
                      .block_size = ResourceConfig::payload_block_size,
                      .high_watermark = resources.buffer_high_watermark,
                      .low_watermark = resources.buffer_low_watermark,
                  },
              .forwarder_buffer_capacity = forwarder_buf_cap,
              .sender_buffer_capacity = sender_buf_cap,
              .buffer_budget = resources.buffer_budget,
          },
          operations.stream, operations.allocation),
      shutdown_signal_fd_(std::move(shutdown_signal_fd)),
      timer_fd_(std::move(timer_fd)),
      deadlines_(operations.allocation),
      timeouts_(timeouts),
      max_connections_(resources.max_connections),
      max_pending_connections_(resources.max_pending_connections),
      time_source_(time_source),
      accept_operations_(operations.accept),
      allocation_(operations.allocation),
      upstream_dialer_(std::move(upstream_dialer)) {}

Result<ProxyReactor, ProxyCreateError>
ProxyReactor::create(const net::ListenSocketAddress& listen_address,
                     const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts,
                     ResourceConfig resources) {
    return createImpl(listen_address, upstream_address, timeouts, resources, {}, {});
}

Result<ProxyReactor, ProxyCreateError>
ProxyReactor::createImpl(const net::ListenSocketAddress& listen_address,
                         const net::ResolutionEndpoint& upstream_address, TimeoutConfig timeouts,
                         ResourceConfig resources, SteadyTimeSource time_source,
                         ReactorOperations operations) {
    if (timeouts.connect.count() < 1 || timeouts.connect > TimeoutConfig::maximum) {
        return std::unexpected(ProxyCreateError{"Invalid connection timeout"});
    }
    if (timeouts.idle.count() < 0 || timeouts.idle > TimeoutConfig::maximum) {
        return std::unexpected(ProxyCreateError{"Invalid idle timeout"});
    }
    if (!resources.validBufferWatermarks()) {
        return std::unexpected(ProxyCreateError{"Invalid directional buffer watermarks"});
    }
    if (!resources.validConnectionLimits()) {
        return std::unexpected(ProxyCreateError{"Invalid connection admission limits"});
    }
    if (!resources.validBufferBudget()) {
        return std::unexpected(ProxyCreateError{"Invalid global forwarding buffer budget"});
    }
    auto poller_create_result = detail::EpollPoller::create(operations.epoll);
    if (!poller_create_result) {
        return std::unexpected(ProxyCreateError{poller_create_result.error().message()});
    }
    detail::EpollPoller poller = std::move(*poller_create_result);

    auto signalfd_create_result = detail::createShutdownSignalFd();
    if (!signalfd_create_result) {
        return std::unexpected(ProxyCreateError{signalfd_create_result.error().message()});
    }
    FileDescriptor signal_fd = std::move(signalfd_create_result.value());

    auto timerfd_create_result = detail::createTimerFd();
    if (!timerfd_create_result) {
        return std::unexpected(ProxyCreateError{timerfd_create_result.error().message()});
    }
    FileDescriptor timer_fd = std::move(timerfd_create_result.value());

    auto dialer_create_result = detail::UpstreamDialer::create(upstream_address);
    if (!dialer_create_result) {
        return std::unexpected(ProxyCreateError{dialer_create_result.error().message});
    }
    detail::UpstreamDialer dialer = std::move(dialer_create_result.value());

    auto listener_create_result = net::Listener::create(listen_address, max_backlog_size);
    if (!listener_create_result) {
        return std::unexpected(ProxyCreateError{listener_create_result.error().message});
    }
    net::Listener listener = std::move(listener_create_result.value());

    ProxyReactor reactor(std::move(poller), std::move(signal_fd), std::move(timer_fd),
                         std::move(dialer), timeouts, resources, time_source, operations);

    auto listener_register_result = reactor.registerListener(listener.fd());
    if (!listener_register_result) {
        return std::unexpected(ProxyCreateError{listener_register_result.error().message()});
    }
    detail::SourceId listener_id = listener_register_result.value();

    reactor.active_listener_ = detail::ActiveListener{
        .listener = std::move(listener),
        .listener_id = listener_id,
    };

    if (auto register_result = reactor.registerShutdownSignalEvent(); !register_result) {
        return std::unexpected(ProxyCreateError{register_result.error().message()});
    }

    if (auto register_result = reactor.registerTimerEvent(); !register_result) {
        return std::unexpected(ProxyCreateError{register_result.error().message()});
    }

    return std::move(reactor);
}

Status<ProxyRuntimeError> ProxyReactor::start() {
    spdlog::info("Starting proxying traffic...");

    while (!shouldStop()) {
        if (auto turn = runTurn(); !turn) {
            return std::unexpected(ProxyRuntimeError{std::move(turn.error().message)});
        }
    }

    if (auto disarmed = synchronizeTimer(); !disarmed) {
        return std::unexpected(ProxyRuntimeError{disarmed.error().message()});
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::runTurn() {
    if (auto armed = synchronizeTimer(); !armed) {
        return std::unexpected(FatalReactorError{armed.error().message()});
    }
    auto events = poller_.wait(hasRunnableDials() || session_manager_.hasReadyRead());
    if (!events) {
        return std::unexpected(FatalReactorError{events.error().message()});
    }
    if (auto dispatched = dispatchBatch(*events); !dispatched || shouldStop()) {
        return dispatched;
    }
    if (auto resumed = resumeDeferredDials(); !resumed) {
        return resumed;
    }
    return resumeReadyRead();
}

Status<FatalReactorError> ProxyReactor::dispatchBatch(std::span<const detail::ReadyEvent> events) {
    size_t dial_attempts_left = max_dial_attempts_per_batch;
    for (const auto& event : events) {
        if (auto status = dispatch(event, dial_attempts_left); !status || shouldStop()) {
            return status;
        }
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::resumeReadyRead() {
    if (auto source = session_manager_.takeReadyRead()) {
        // This also retries a hung-up source whose readable edge was already consumed.
        size_t no_dial_attempts = 0;
        return dispatch({*source, EPOLLIN}, no_dial_attempts, true);
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::dispatch(detail::ReadyEvent event,
                                                 size_t& dial_attempts_left, bool from_wait_queue) {
    auto registration = sources_.find(event.source_id);

    if (!registration) {
        return {};
    }

    auto status = std::visit(
        overloaded{
            [&](detail::EndpointRegistration r) {
                return handleEndpoint(r, event.events, from_wait_queue);
            },
            [&](detail::ListenerRegistration r) -> Status<FatalReactorError> {
                if (event.events & (EPOLLERR | EPOLLHUP)) {
                    return std::unexpected(FatalReactorError{"Listener reported error or hangup"});
                }
                return handleListener(r, dial_attempts_left);
            },
            [&](detail::ShutdownSignalRegistration r) { return handleShutdownSignal(r); },
            [&](detail::TimerRegistration r) { return handleTimer(r); },
            [&](detail::PendingDialRegistration) {
                return handlePendingConnection(event.source_id, dial_attempts_left);
            },
        },
        *registration);
    if (!status) {
        return status;
    }
    if (auto interests = synchronizeListener(); !interests) {
        return std::unexpected(FatalReactorError{interests.error().message()});
    }
    return {};
}

Status<std::error_code> ProxyReactor::closeSession(detail::SessionId session_id) {
    if (!session_manager_.contains(session_id)) {
        return {};
    }

    if (timeouts_.idle.count() > 0) {
        deadlines_.cancel({detail::DeadlineKind::Idle, session_id});
    }
    detail::SessionSourceIds source_ids = session_manager_.sourceIds(session_id);
    std::optional<std::error_code> first_error;

    for (auto id : {source_ids.downstream, source_ids.upstream}) {
        deadlines_.cancel({detail::DeadlineKind::ResidualRead, id});
        deadlines_.cancel({detail::DeadlineKind::FinishWrite, id});
        if (poller_.isWatching(id)) {
            if (auto result = unregisterReactorSource(id); !result && !first_error) {
                first_error = result.error();
            }
        }
    }

    session_manager_.remove(session_id);
    if (shutdown_state_ == ShutdownState::GracefullyStopping && !hasActiveSessions()) {
        deadlines_.cancel(shutdown_deadline);
    }

    if (first_error) {
        return std::unexpected(first_error.value());
    }

    return {};
}

bool ProxyReactor::hasActiveSessions() const { return !session_manager_.empty(); }

Result<detail::SourceId, std::error_code>
ProxyReactor::registerReactorSource(int fd, uint32_t initial_interests,
                                    const detail::ReactorRegistration& reactor_registration) {
    detail::SourceId source_id = sources_.add(reactor_registration);

    if (auto result = poller_.add(source_id, fd, initial_interests); !result) {
        sources_.remove(source_id);
        return std::unexpected(result.error());
    }

    return source_id;
}

// If everything succeeds then the endpoint is registered and the resulting state is consistent.
// If anything fails the object's state remains as if this function was not called.
Result<detail::SourceId, std::error_code>
ProxyReactor::registerEndpoint(int fd, uint32_t initial_interests, detail::SessionId session_id,
                               detail::EndpointRole role) {
    detail::EndpointRegistration registration = {
        .session_id = session_id,
        .role = role,
    };

    return registerReactorSource(fd, initial_interests, registration);
}

Status<std::error_code> ProxyReactor::unregisterReactorSource(detail::SourceId id) {
    auto result = poller_.retire(id);
    sources_.remove(id);
    return result;
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerListener(int fd) {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(fd, initial_events, detail::ListenerRegistration{});
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerShutdownSignalEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(shutdown_signal_fd_.get(), initial_events,
                                 detail::ShutdownSignalRegistration{});
}

Result<detail::SourceId, std::error_code> ProxyReactor::registerTimerEvent() {
    uint32_t initial_events = EPOLLIN;

    return registerReactorSource(timer_fd_.get(), initial_events, detail::TimerRegistration{});
}

Result<detail::SourceId, ConnectionFailure>
ProxyReactor::registerPendingConnection(detail::PendingConnection pending_connection,
                                        AdmissionReservation& admission, bool interrupted) {
    const bool watched = pending_connection.attempted_connection_fd.has_value() && !interrupted;
    Result<detail::SourceId, std::error_code> registered =
        watched ? registerReactorSource(pending_connection.attempted_connection_fd->get(), EPOLLOUT,
                                        detail::PendingDialRegistration{})
                : Result<detail::SourceId, std::error_code>(sources_.allocateId());
    if (!registered) {
        return std::unexpected(
            ConnectionFailure{ConnectionOperation::AddWatch, registered.error()});
    }
    const detail::DeadlineKey key{detail::DeadlineKind::Connect, *registered};
    const detail::DeadlineKey retry_key{detail::DeadlineKind::DialRetry, *registered};
    std::optional<QueuedDial> deferred;
    ConnectionOperation allocation = ConnectionOperation::AllocateDeadline;
    try {
        deadlines_.schedule(key, pending_connection.deadline);
        if (interrupted) {
            deadlines_.schedule(retry_key, time_source_.now() + interrupted_io_retry_interval);
        } else if (!watched) {
            allocation = ConnectionOperation::AllocateDeferredDial;
            deferred = enqueueDeferredDial(*registered, pending_connection);
        }
    } catch (const std::bad_alloc&) {
        deadlines_.cancel(key);
        deadlines_.cancel(retry_key);
        Status<std::error_code> retired;
        if (watched) {
            retired = unregisterReactorSource(*registered);
        }
        return std::unexpected(ConnectionFailure{allocation,
                                                 std::make_error_code(std::errc::not_enough_memory),
                                                 retired ? std::error_code{} : retired.error()});
    }
    // Abseil table allocation failures remain fatal.
    pending_connections_.emplace(*registered,
                                 PendingEntry{std::move(pending_connection), deferred, watched});
    admission.release();
    return *registered;
}

Result<ProxyReactor::ExtractedPending, std::error_code>
ProxyReactor::takePending(detail::SourceId id) {
    auto it = pending_connections_.find(id);
    assert(it != pending_connections_.end());
    deadlines_.cancel({detail::DeadlineKind::Connect, id});
    deadlines_.cancel({detail::DeadlineKind::DialRetry, id});
    Status<std::error_code> retired;
    if (auto deferred = it->second.deferred) {
        deferredQueue(deferred->kind).erase(deferred->position);
    } else if (it->second.watched) {
        retired = unregisterReactorSource(id);
    }
    // Transfer the charge with the owner. No callback or admission check runs during the move.
    ExtractedPending extracted{AdmissionReservation(transient_admission_),
                               std::move(it->second.connection)};
    pending_connections_.erase(it);
    if (!retired) {
        return std::unexpected(retired.error());
    }
    return extracted;
}

Status<std::error_code> ProxyReactor::cancelPendingConnections() {
    Status<std::error_code> status;
    for (auto it = pending_connections_.begin(); it != pending_connections_.end();) {
        auto id = (it++)->first; // takePending erases only this entry.
        if (auto pending = takePending(id); !pending && status) {
            status = std::unexpected(pending.error());
        }
    }
    return status;
}

// If everything succeeds both endpoints are registered and SessionManager assumes ownership of
// their file descriptors. If registration fails, the reactor source changes are rolled back.
Result<detail::SessionId, ConnectionFailure>
ProxyReactor::addSession(FileDescriptor upstream_fd, FileDescriptor downstream_fd,
                         detail::Deadline deadline, AdmissionReservation& admission) {
    auto session = session_manager_.prepare(std::move(downstream_fd), std::move(upstream_fd));
    if (!session) {
        return std::unexpected(
            ConnectionFailure{ConnectionOperation::AllocateBuffers,
                              std::make_error_code(std::errc::not_enough_memory)});
    }
    // Abseil table allocation failures remain fatal. Reserve before the handoff timestamp.
    session_manager_.reserveForAdd();
    const auto id = session_manager_.allocateId();
    const detail::DeadlineKey idle_key{detail::DeadlineKind::Idle, id};
    constexpr uint32_t initial_events = EPOLLIN | EPOLLRDHUP;
    auto downstream = registerEndpoint(session->downstream.fd.get(), initial_events, id,
                                       detail::EndpointRole::Downstream);
    if (!downstream) {
        return std::unexpected(
            ConnectionFailure{ConnectionOperation::AddWatch, downstream.error()});
    }
    auto upstream = registerEndpoint(session->upstream.fd.get(), initial_events, id,
                                     detail::EndpointRole::Upstream);
    auto rollback = [&](ConnectionFailure failure) {
        deadlines_.cancel(idle_key);
        if (upstream) {
            if (auto status = unregisterReactorSource(*upstream); !status) {
                failure.rollback_error = status.error();
            }
        }
        if (auto status = unregisterReactorSource(*downstream);
            !status && !failure.rollback_error) {
            failure.rollback_error = status.error();
        }
        return std::unexpected(failure);
    };
    if (!upstream) {
        return rollback({ConnectionOperation::AddWatch, upstream.error()});
    }

    const bool idle_enabled = timeouts_.idle.count() > 0;
    if (idle_enabled) {
        try {
            // Prepare both deadline indexes. Updating this key at handoff reuses its nodes.
            deadlines_.schedule(idle_key, detail::Deadline::max());
        } catch (const std::bad_alloc&) {
            return rollback({ConnectionOperation::AllocateDeadline,
                             std::make_error_code(std::errc::not_enough_memory)});
        }
    }
    const auto handoff = time_source_.now();
    if (handoff >= deadline) {
        return rollback({ConnectionOperation::EstablishmentDeadline,
                         std::make_error_code(std::errc::timed_out)});
    }
    if (idle_enabled) {
        deadlines_.schedule(idle_key, handoff + timeouts_.idle);
    }
    session_manager_.add(id, std::move(*session), {*downstream, *upstream});
    admission.release();
    return id;
}

Status<FatalReactorError>
ProxyReactor::handleEndpoint(detail::EndpointRegistration registration, uint32_t event_mask,
                             bool from_wait_queue, std::optional<detail::SessionRetryKind> retry) {
    detail::SessionId session_id = registration.session_id;

    if (!session_manager_.contains(session_id)) {
        return {};
    }

    std::optional<detail::IdleDeadline> idle_deadline;
    if (timeouts_.idle.count() > 0) {
        auto at = deadlines_.scheduledAt({detail::DeadlineKind::Idle, session_id});
        idle_deadline.emplace(at.value(), timeouts_.idle, time_source_);
    }
    auto* idle = idle_deadline ? &*idle_deadline : nullptr;

    if (retry) {
        auto result =
            session_manager_.retryInterrupted(session_id, {registration.role, *retry}, idle);
        if (auto applied = applySessionEventResult(session_id, result, idle); !applied) {
            return std::unexpected(applied.error());
        }
        return {};
    }

    // Error and full hangup supersede ordinary readiness in the same notification.
    // A full hangup has its own bounded read and cannot accept queued output.
    if (event_mask & (EPOLLERR | EPOLLHUP)) {
        if (!(event_mask & EPOLLERR)) {
            const auto [downstream, upstream] = session_manager_.sourceIds(session_id);
            const bool from_client = registration.role == detail::EndpointRole::Downstream;
            deadlines_.cancel(
                {detail::DeadlineKind::FinishWrite, from_client ? downstream : upstream});
            deadlines_.cancel(
                {detail::DeadlineKind::ResidualRead, from_client ? upstream : downstream});
        }
        auto result = (event_mask & EPOLLERR)
                          ? session_manager_.handleError(session_id, registration.role)
                          : session_manager_.handleHangup(session_id, registration.role, idle);
        if (auto applied = applySessionEventResult(session_id, result, idle); !applied) {
            return std::unexpected(applied.error());
        }
        return {};
    }

    for (auto event_flag : {EPOLLIN, EPOLLOUT, EPOLLRDHUP}) {
        if (!(event_mask & event_flag)) {
            continue;
        }

        detail::SessionEventResult action;
        switch (event_flag) {
        case EPOLLIN:
            action = session_manager_.handleReadable(session_id, registration.role, idle,
                                                     from_wait_queue);
            break;
        case EPOLLOUT:
            action = session_manager_.handleWritable(session_id, registration.role, idle);
            break;
        default:
            action = session_manager_.handlePeerHalfClosed(session_id, registration.role, idle);
            break;
        }
        auto result = applySessionEventResult(session_id, action, idle);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == EndpointEventOutcome::SessionClosed ||
            session_manager_.shouldRetire(session_id, registration.role)) {
            return {};
        }
    }

    return {};
}

Result<ProxyReactor::EndpointEventOutcome, FatalReactorError>
ProxyReactor::applySessionEventResult(detail::SessionId session_id,
                                      const detail::SessionEventResult& event_result,
                                      detail::IdleDeadline* idle) {
    if (event_result.error) {
        spdlog::error("Error in session {}: {}", session_id, *event_result.error);
    }

    if (event_result.action == detail::SessionEventAction::Close || (idle && idle->expired())) {
        if (auto closed = closeSession(session_id); !closed) {
            return std::unexpected(FatalReactorError{closed.error().message()});
        }
        return EndpointEventOutcome::SessionClosed;
    }

    if (idle) {
        if (auto at = idle->takeRefresh()) {
            deadlines_.schedule({detail::DeadlineKind::Idle, session_id}, *at);
        }
    }
    if (auto retry = event_result.retry) {
        const auto [downstream, upstream] = session_manager_.sourceIds(session_id);
        const auto source = retry->role == detail::EndpointRole::Downstream ? downstream : upstream;
        const auto kind = retry->kind == detail::SessionRetryKind::ResidualRead
                              ? detail::DeadlineKind::ResidualRead
                              : detail::DeadlineKind::FinishWrite;
        try {
            deadlines_.schedule({kind, source}, time_source_.now() + interrupted_io_retry_interval);
        } catch (const std::bad_alloc&) {
            if (auto closed = closeSession(session_id); !closed) {
                return std::unexpected(FatalReactorError{closed.error().message()});
            }
            return EndpointEventOutcome::SessionClosed;
        }
    }
    if (auto result = synchronizeSessionInterests(session_id); !result) {
        if (auto closed = closeSession(session_id); !closed) {
            return std::unexpected(FatalReactorError{closed.error().message()});
        }
        if (auto recovered = handleConnectionFailure(result.error()); !recovered) {
            return std::unexpected(recovered.error());
        }
        return EndpointEventOutcome::SessionClosed;
    }

    return EndpointEventOutcome::KeepSession;
}

Status<ConnectionFailure> ProxyReactor::synchronizeSessionInterests(detail::SessionId session_id) {
    detail::SessionSourceIds source_ids = session_manager_.sourceIds(session_id);
    detail::SessionInterests interests = session_manager_.interests(session_id);

    if (auto result = synchronizeEndpoint(session_id, detail::EndpointRole::Downstream,
                                          source_ids.downstream, interests.downstream);
        !result) {
        return result;
    }

    return synchronizeEndpoint(session_id, detail::EndpointRole::Upstream, source_ids.upstream,
                               interests.upstream);
}

Status<ConnectionFailure>
ProxyReactor::synchronizeEndpoint(detail::SessionId session_id, detail::EndpointRole role,
                                  detail::SourceId source_id,
                                  const detail::EndpointInterests& interests) {
    // After residual EOF the source no longer needs even an edge-triggered watch.
    if (session_manager_.shouldRetire(session_id, role)) {
        if (!poller_.isWatching(source_id)) {
            return {};
        }
        if (auto result = unregisterReactorSource(source_id); !result) {
            return std::unexpected(
                ConnectionFailure{ConnectionOperation::RemoveWatch, result.error()});
        }
        return {};
    }

    if (auto result = poller_.setInterests(source_id, getEventMask(interests)); !result) {
        return std::unexpected(ConnectionFailure{ConnectionOperation::ChangeWatch, result.error()});
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::handleShutdownSignal(detail::ShutdownSignalRegistration) {
    auto drain_result = detail::readShutdownRequests(shutdown_signal_fd_.get());
    if (!drain_result) {
        return std::unexpected(FatalReactorError{drain_result.error().message()});
    }
    int count = drain_result.value();

    for (int i = 0; i < count; i++) {
        if (auto shutdown_request_result = handleShutdownRequest(); !shutdown_request_result) {
            return std::unexpected(FatalReactorError{shutdown_request_result.error().message()});
        }
    }

    return {};
}

Status<FatalReactorError> ProxyReactor::handleTimer(detail::TimerRegistration) {
    auto expired = detail::drainTimerFd(timer_fd_.get());
    if (!expired) {
        return std::unexpected(FatalReactorError{expired.error().message()});
    }
    if (*expired) {
        armed_deadline_.reset();
        return expireDeadlines(time_source_.now());
    }
    return {};
}

Status<std::error_code> ProxyReactor::synchronizeTimer() {
    // A simulated time point has no meaning to the kernel's CLOCK_MONOTONIC timer.
    if (!time_source_.usesSystemClock()) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
    auto next = nextWakeup();
    if (next != armed_deadline_) {
        if (auto result = detail::setTimer(timer_fd_.get(), next); !result) {
            return result;
        }
        armed_deadline_ = next;
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::expireDeadlines(detail::Deadline now) {
    // A burst of earlier idle expirations must not defer a due process shutdown.
    if (shutdown_state_ == ShutdownState::GracefullyStopping &&
        deadlines_.due(shutdown_deadline, now)) {
        if (auto shutdown = handleShutdownRequest(); !shutdown) {
            return std::unexpected(FatalReactorError{shutdown.error().message()});
        }
        return {};
    }
    if (admission_retry_at_ && *admission_retry_at_ <= now) {
        admission_retry_at_.reset();
    }
    for (int i = 0; i < max_expirations_per_dispatch; ++i) {
        auto key = deadlines_.popDue(now);
        if (!key) {
            break;
        }
        if (key->kind == detail::DeadlineKind::Idle) {
            if (auto closed = closeSession(key->id); !closed) {
                return std::unexpected(FatalReactorError{closed.error().message()});
            }
        } else if (key->kind == detail::DeadlineKind::Connect &&
                   pending_connections_.contains(key->id)) {
            auto pending = takePending(key->id);
            if (!pending) {
                return std::unexpected(FatalReactorError{pending.error().message()});
            }
            spdlog::debug("Upstream connection establishment timed out");
        } else if (key->kind == detail::DeadlineKind::DialRetry) {
            auto it = pending_connections_.find(key->id);
            if (it != pending_connections_.end()) {
                try {
                    it->second.deferred = enqueueDeferredDial(key->id, it->second.connection);
                } catch (const std::bad_alloc&) {
                    if (auto dropped = takePending(key->id); !dropped) {
                        return std::unexpected(FatalReactorError{dropped.error().message()});
                    }
                    suspendAdmission("allocate deferred dial entry",
                                     std::make_error_code(std::errc::not_enough_memory), false);
                }
            }
        } else if (key->kind == detail::DeadlineKind::ResidualRead ||
                   key->kind == detail::DeadlineKind::FinishWrite) {
            auto registration = sources_.find(key->id);
            if (!registration) {
                continue;
            }
            auto* endpoint = std::get_if<detail::EndpointRegistration>(&*registration);
            if (endpoint) {
                auto retry = key->kind == detail::DeadlineKind::ResidualRead
                                 ? detail::SessionRetryKind::ResidualRead
                                 : detail::SessionRetryKind::FinishWrite;
                if (auto resumed = handleEndpoint(*endpoint, 0, false, retry); !resumed) {
                    return resumed;
                }
            }
        }
    }
    return {};
}

std::optional<detail::Deadline> ProxyReactor::nextWakeup() const {
    const auto next = deadlines_.next();
    if (admission_retry_at_ && (!next || *admission_retry_at_ < *next)) {
        return admission_retry_at_;
    }
    return next;
}

void ProxyReactor::suspendAdmission(const char* operation, std::error_code code, bool report) {
    if (shutdown_state_ != ShutdownState::Running) {
        return;
    }
    if (!admission_retry_at_) {
        admission_retry_at_ = time_source_.now() + admission_retry_interval;
    }
    if (report && !admission_pause_reported_) {
        admission_pause_reported_ = true;
        spdlog::warn("Admission paused after {} failed (errno {})", operation, code.value());
    }
}

size_t ProxyReactor::pendingConnectionCount() const {
    return pending_connections_.size() + transient_admission_;
}

bool ProxyReactor::canAdmit() const {
    const size_t pending = pendingConnectionCount();
    return shutdown_state_ == ShutdownState::Running && !admission_retry_at_ &&
           pending < max_pending_connections_ &&
           session_manager_.size() + pending < max_connections_;
}

Status<std::error_code> ProxyReactor::synchronizeListener() {
    if (!active_listener_) {
        return {};
    }
    const bool enabled = canAdmit();
    if (enabled == active_listener_->read_enabled) {
        return {};
    }
    auto result = poller_.setInterests(active_listener_->listener_id, enabled ? EPOLLIN : 0);
    if (result) {
        active_listener_->read_enabled = enabled;
    }
    return result;
}

Status<FatalReactorError> ProxyReactor::handleListener(detail::ListenerRegistration,
                                                       size_t& attempts_left) {
    for (int i = 0; i < max_accept_batch_size && attempts_left > 0 && canAdmit(); ++i) {
        AdmissionReservation reservation(transient_admission_);
        auto accept_result = active_listener_->listener.acceptClientConnection(accept_operations_);
        if (!accept_result) {
            auto error = accept_result.error();
            switch (classifyAcceptError(error)) {
            case AcceptFailure::Connection:
                spdlog::debug("Connection could not be accepted (errno {})", error.value());
                continue;
            case AcceptFailure::Backoff:
                suspendAdmission("accept4", error);
                return {};
            case AcceptFailure::Infrastructure:
                return std::unexpected(
                    FatalReactorError{std::format("Listener accept failed: {}", error.message())});
            }
        }

        if (std::holds_alternative<net::AcceptWouldBlock>(*accept_result)) {
            return {};
        }

        auto accept_success = std::get<net::AcceptSuccess>(std::move(*accept_result));

        spdlog::info("Client {} connected", net::formatAddress(accept_success.remote));

        auto deadline = time_source_.now() + timeouts_.connect;
        auto dial_result =
            upstream_dialer_.dial(std::move(accept_success.fd), accept_success.remote, deadline,
                                  time_source_, attempts_left);
        if (!dial_result) {
            if (auto error_result = handleDialError(std::move(dial_result.error()));
                !error_result) {
                return error_result;
            }

            continue;
        }

        if (auto dispatch_result = handleDialResult(std::move(*dial_result), reservation);
            !dispatch_result) {
            return dispatch_result;
        }
    }

    return {};
}

Status<FatalReactorError> ProxyReactor::handlePendingConnection(detail::SourceId id,
                                                                size_t& attempts_left) {
    auto pending = takePending(id);
    if (!pending) {
        return std::unexpected(FatalReactorError{pending.error().message()});
    }
    if (shutdown_state_ != ShutdownState::Running) {
        return {};
    }
    size_t no_attempts = 0;
    auto dial_result =
        upstream_dialer_.advanceDialAttempt(std::move(pending->connection), time_source_,
                                            admission_retry_at_ ? no_attempts : attempts_left);
    if (!dial_result) {
        return handleDialError(std::move(dial_result.error()));
    }

    return handleDialResult(std::move(*dial_result), pending->admission);
}

bool ProxyReactor::hasRunnableDials() const {
    return shutdown_state_ == ShutdownState::Running &&
           (!deferred_completions_.empty() ||
            (!admission_retry_at_ && !deferred_attempts_.empty()));
}

ProxyReactor::DeferredDials& ProxyReactor::deferredQueue(DeferredDialKind kind) {
    return kind == DeferredDialKind::CompletionQuery ? deferred_completions_ : deferred_attempts_;
}

ProxyReactor::QueuedDial
ProxyReactor::enqueueDeferredDial(detail::SourceId id, const detail::PendingConnection& pending) {
    allocation_.check(detail::AllocationPoint::DeferredDial);
    const auto kind = pending.attempted_connection_fd ? DeferredDialKind::CompletionQuery
                                                      : DeferredDialKind::SocketAttempt;
    auto& queue = deferredQueue(kind);
    return {kind, queue.insert(queue.end(), id)};
}

Status<FatalReactorError> ProxyReactor::resumeDeferredDials() {
    // Give each queued dial at most one opportunity this turn, alternating eligible queues.
    // A completion that needs a fresh socket joins the attempt FIFO for a later turn.
    size_t attempts = deferred_attempts_.size();
    size_t completions = deferred_completions_.size();
    for (size_t i = 0; i < max_resumed_dials_per_turn && shutdown_state_ == ShutdownState::Running;
         ++i) {
        const bool attempt_ready = attempts > 0 && !admission_retry_at_;
        if (!attempt_ready && completions == 0) {
            break;
        }
        const bool complete = completions > 0 && (!attempt_ready || i % 2 == 0);
        --(complete ? completions : attempts);
        auto& queue = complete ? deferred_completions_ : deferred_attempts_;
        const auto id = queue.front();
        auto& entry = pending_connections_.at(id);
        size_t attempts_left = admission_retry_at_ ? 0 : 1;
        auto result = complete ? upstream_dialer_.advanceDialAttempt(std::move(entry.connection),
                                                                     time_source_, attempts_left)
                               : upstream_dialer_.resumeDial(std::move(entry.connection),
                                                             time_source_, attempts_left);
        if (result) {
            if (auto* deferred = std::get_if<detail::UpstreamDialDeferred>(&*result);
                deferred && !deferred->interrupted) {
                entry.connection = std::move(deferred->pending_connection);
                assert(!entry.connection.attempted_connection_fd);
                deferred_attempts_.splice(deferred_attempts_.end(), queue,
                                          entry.deferred->position);
                entry.deferred->kind = DeferredDialKind::SocketAttempt;
                continue;
            }
        }
        // A successful result already holds the sockets. Retire the old record before handoff.
        auto removed = takePending(id);
        assert(removed && "deferred entries have no watch to retire");
        auto handled = result ? handleDialResult(std::move(*result), removed->admission)
                              : handleDialError(result.error());
        if (!handled) {
            return handled;
        }
    }
    if (auto updated = synchronizeListener(); !updated) {
        return std::unexpected(FatalReactorError{updated.error().message()});
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::handleDialResult(detail::UpstreamDialResult dial_result,
                                                         AdmissionReservation& admission) {
    if (auto* connected = std::get_if<detail::UpstreamDialConnected>(&dial_result)) {
        spdlog::info("Connected to {} on behalf of client {}",
                     net::formatAddress(connected->upstream_address),
                     net::formatAddress(connected->accepted_address));

        if (auto session_add_result = addSession(std::move(connected->upstream_connection_fd),
                                                 std::move(connected->accepted_connection_fd),
                                                 connected->deadline, admission);
            !session_add_result) {

            return handleConnectionFailure(session_add_result.error());
        }

        spdlog::info("Session between {} and {} established",
                     net::formatAddress(connected->accepted_address),
                     net::formatAddress(connected->upstream_address));

    } else if (auto* in_progress = std::get_if<detail::UpstreamDialInProgress>(&dial_result)) {
        if (time_source_.now() >= in_progress->pending_connection.deadline) {
            spdlog::debug("Upstream connection establishment timed out");
            return {};
        }
        net::SocketAddress accepted_address = in_progress->pending_connection.accepted_address;

        if (auto register_result =
                registerPendingConnection(std::move(in_progress->pending_connection), admission);
            !register_result) {
            return handleConnectionFailure(register_result.error());
        }

        spdlog::debug("Pending connection for {} registered", net::formatAddress(accepted_address));

    } else {
        auto& deferred = std::get<detail::UpstreamDialDeferred>(dial_result);
        if (time_source_.now() >= deferred.pending_connection.deadline) {
            return {};
        }
        if (auto registered = registerPendingConnection(std::move(deferred.pending_connection),
                                                        admission, deferred.interrupted);
            !registered) {
            return handleConnectionFailure(registered.error());
        }
        return {};
    }

    if (!admission_retry_at_) {
        admission_pause_reported_ = false;
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::handleDialError(net::DialError error) {
    const char* operation = net::dialOperationName(error.failed_op);
    switch (net::classifyDialError(error)) {
    case net::DialFailureKind::Infrastructure:
        return std::unexpected(
            FatalReactorError{std::format("{} failed: {}", operation, error.code.message())});
    case net::DialFailureKind::Pressure:
        suspendAdmission(operation, error.code);
        break;
    case net::DialFailureKind::Timeout:
    case net::DialFailureKind::Candidate:
        const auto& upstream = upstream_dialer_.upstreamEndpoint();
        spdlog::debug("Dial to {}:{} ended at {} (errno {})", upstream.hostname, upstream.port,
                      operation, error.code.value());
        break;
    }
    return {};
}

Status<FatalReactorError> ProxyReactor::handleConnectionFailure(ConnectionFailure failure) {
    const char* operation = operationName(failure.operation);
    if (failure.rollback_error) {
        return std::unexpected(FatalReactorError{
            std::format("{} failed (errno {}) and registration rollback failed (errno {})",
                        operation, failure.code.value(), failure.rollback_error.value())});
    }
    switch (failure.operation) {
    case ConnectionOperation::AllocateBuffers:
    case ConnectionOperation::AllocateDeadline:
    case ConnectionOperation::AllocateDeferredDial:
        suspendAdmission(operation, failure.code, false);
        return {};
    case ConnectionOperation::EstablishmentDeadline:
        return {};
    case ConnectionOperation::AddWatch:
    case ConnectionOperation::ChangeWatch:
        if (failure.code.value() == ENOSPC || failure.code.value() == ENOMEM) {
            suspendAdmission(operation, failure.code);
            return {};
        }
        if (failure.code.value() == EPERM ||
            (failure.operation == ConnectionOperation::ChangeWatch &&
             failure.code.category() == std::system_category() && failure.code.value() == ENOENT)) {
            spdlog::debug("{} failed for connection (errno {})", operation, failure.code.value());
            return {};
        }
        break;
    case ConnectionOperation::RemoveWatch:
        break;
    }
    return std::unexpected(
        FatalReactorError{std::format("{} failed: {}", operation, failure.code.message())});
}

Status<std::error_code> ProxyReactor::handleShutdownRequest() {
    switch (shutdown_state_) {
    case ShutdownState::Running:
        if (hasActiveSessions()) {
            deadlines_.schedule(shutdown_deadline, time_source_.now() + graceful_shutdown_timeout);
        }
        if (auto unregister_listener_result =
                unregisterReactorSource(active_listener_->listener_id);
            !unregister_listener_result) {
            return unregister_listener_result;
        }
        active_listener_.reset();
        admission_retry_at_.reset();
        shutdown_state_ = ShutdownState::GracefullyStopping;
        return cancelPendingConnections();
    case ShutdownState::GracefullyStopping:
        deadlines_.cancel(shutdown_deadline);
        shutdown_state_ = ShutdownState::HardStopping;
        return forceCloseAllSessions();
    case ShutdownState::HardStopping:
        // No-op
        break;
    }

    return {};
}

bool ProxyReactor::shouldStop() const {
    if (shutdown_state_ == ShutdownState::HardStopping) {
        return true;
    }

    if (shutdown_state_ == ShutdownState::GracefullyStopping && !hasActiveSessions()) {
        return true;
    }

    return false;
}

Status<std::error_code> ProxyReactor::forceCloseAllSessions() {
    std::optional<std::error_code> first_error;

    const auto ids = session_manager_.ids();
    for (auto it = ids.begin(); it != ids.end();) {
        const auto session_id = *it++; // Advance before closeSession erases the current owner.

        if (auto result = closeSession(session_id); !result) {
            spdlog::warn("Failed to close session {} during hard shutdown: {}", session_id,
                         result.error().message());

            if (!first_error) {
                first_error = result.error();
            }
        }
    }

    if (first_error) {
        return std::unexpected(first_error.value());
    }

    return {};
}

} // namespace orbit::proxy
