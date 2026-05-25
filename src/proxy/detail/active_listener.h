#pragma once

#include "net/listener.h"
#include "proxy/detail/reactor_types.h"

namespace orbit::proxy::detail {

struct ActiveListener {
    net::Listener listener;
    detail::ReactorSourceId listener_id;
};

} // namespace orbit::proxy::detail
