#pragma once

#include <memory>
#include <variant>

#include "common/fd.h"
#include "proxy/detail/generator.h"
#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

// TODO: Document the structs

using SessionId = MonotonicIdGenerator::Id;
using ReactorSourceId = MonotonicIdGenerator::Id;

// Owns resources associated with the session.
struct ProxySession {
    FileDescriptor downstream_fd;
    FileDescriptor upstream_fd;
    std::unique_ptr<SessionPair> endpoints;
};

// Reactor's view of a managed session.
struct ManagedSession {
    ProxySession session;
    ReactorSourceId downstream_endpoint_id;
    ReactorSourceId upstream_endpoint_id;
};

enum class EndpointRole {
    Downstream,
    Upstream,
};

struct EndpointRegistration {
    SessionId session_id;
    EndpointRole role;
};

struct ListenerRegistration {};

struct ShutdownSignalRegistration {};

using ReactorRegistration =
    std::variant<EndpointRegistration, ListenerRegistration, ShutdownSignalRegistration>;

} // namespace orbit::proxy::detail
