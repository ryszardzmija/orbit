#pragma once

#include "net/listener.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

struct ActiveListener {
    net::Listener listener;
    detail::SourceId listener_id;
};

} // namespace orbit::proxy::detail
