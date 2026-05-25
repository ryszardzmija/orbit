#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

#include "proxy/detail/reactor_types.h"
#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

std::expected<void, std::error_code> modifyEpollEvents(ReactorSourceId source_id,
                                                       SessionEndpoint& endpoint, int fd, int epfd,
                                                       uint32_t new_events,
                                                       uint32_t current_events);

}
