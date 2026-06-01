#pragma once

#include "proxy/detail/session/session_id.h"

namespace orbit::proxy::detail {

enum class EndpointRole {
    Downstream,
    Upstream,
};

struct EndpointRegistration {
    SessionId session_id;
    EndpointRole role;
};

} // namespace orbit::proxy::detail
