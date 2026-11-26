#pragma once

#include "net/listener.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

struct ActiveListener {
    net::Listener listener;
    detail::SourceId listener_id;
    bool read_enabled = true;
};

} // namespace orbit::proxy::detail
