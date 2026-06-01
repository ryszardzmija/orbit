#pragma once

#include "proxy/detail/dial/connection_id.h"

namespace orbit::proxy::detail {

struct PendingDialRegistration {
    PendingConnectionId pending_connection_id;
};

} // namespace orbit::proxy::detail
