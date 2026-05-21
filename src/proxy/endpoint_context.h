#pragma once

#include "proxy/generator.h"
#include "proxy/session_pair.h"

namespace orbit {

using EndpointId = MonotonicIdGenerator::Id;

struct EndpointContext {
    SessionEndpoint& endpoint;
    EndpointId endpoint_id;
    EndpointId other_endpoint_id;
};

} // namespace orbit
