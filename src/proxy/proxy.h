#pragma once

#include <expected>
#include <system_error>
#include <unordered_map>

#include "common/fd.h"
#include "proxy/endpoint_context.h"
#include "proxy/generator.h"
#include "proxy/session_pair.h"

namespace orbit {

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

    using SessionId = MonotonicIdGenerator::Id;

    class SessionIdGenerator {
    public:
        SessionIdGenerator() = default;

        SessionId getNextId() { return generator_.getNextId(); }

    private:
        MonotonicIdGenerator generator_;
    };

    class EndpointIdGenerator {
    public:
        EndpointIdGenerator() = default;

        EndpointId getNextId() { return generator_.getNextId(); }

    private:
        MonotonicIdGenerator generator_;
    };

    // Owns resources associated with the session.
    struct ProxySession {
        FileDescriptor downstream_fd;
        FileDescriptor upstream_fd;
        std::unique_ptr<SessionPair> endpoints;
    };

    // Reactor's view of a managed session.
    struct ManagedSession {
        ProxySession session;
        EndpointId downstream_endpoint_id;
        EndpointId upstream_endpoint_id;
    };

    enum class EndpointRole {
        Downstream,
        Upstream,
    };

    // Reactor's record of a registered endpoint.
    struct EndpointRegistration {
        SessionId session_id;
        EndpointRole role;
    };

    ProxyReactor(FileDescriptor epfd, std::unordered_map<SessionId, ManagedSession> sessions,
                 std::unordered_map<EndpointId, EndpointRegistration> endpoint_registrations,
                 SessionIdGenerator session_id_generator,
                 EndpointIdGenerator endpoint_id_generator);

    std::expected<void, std::error_code> closeSession(SessionId session_id);
    void closeSessionAndLog(SessionId session_id);
    SessionEndpoint& getEndpoint(ManagedSession& managed_session, EndpointRole role);
    EndpointId getOtherEndpointId(ManagedSession& managed_session, EndpointRole role);

    FileDescriptor epfd_;
    std::unordered_map<SessionId, ManagedSession> sessions_;
    std::unordered_map<EndpointId, EndpointRegistration> endpoint_registrations_;
    SessionIdGenerator session_id_generator_;
    EndpointIdGenerator endpoint_id_generator_;
};

} // namespace orbit
