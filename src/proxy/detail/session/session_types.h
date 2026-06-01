#pragma once

#include <memory>

#include "common/fd.h"
#include "proxy/detail/session_pair.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

// Owns resources associated with the session.
struct ProxySession {
    FileDescriptor downstream_fd;
    FileDescriptor upstream_fd;
    std::unique_ptr<SessionPair> endpoints;
};

// Reactor's view of a managed session.
struct ManagedSession {
    ProxySession session;
    SourceId downstream_endpoint_id;
    SourceId upstream_endpoint_id;
};

} // namespace orbit::proxy::detail
