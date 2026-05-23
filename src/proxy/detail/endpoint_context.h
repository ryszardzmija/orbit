#pragma once

#include "proxy/detail/reactor_types.h"
#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

struct EndpointContext {
    SessionEndpoint& endpoint;
    ReactorSourceId endpoint_id;
    ReactorSourceId other_endpoint_id;
};

} // namespace orbit::proxy::detail
