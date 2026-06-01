#pragma once

#include <variant>

#include "proxy/detail/dial/dial_registration.h"
#include "proxy/detail/listener/listener_registration.h"
#include "proxy/detail/session/session_registration.h"
#include "proxy/detail/shutdown/shutdown_registration.h"

namespace orbit::proxy::detail {

using ReactorRegistration =
    std::variant<EndpointRegistration, ListenerRegistration, ShutdownSignalRegistration,
                 ShutdownTimerRegistration, PendingDialRegistration>;

}
