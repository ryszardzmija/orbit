#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

#include "proxy/endpoint_context.h"

namespace orbit::proxy {

std::expected<void, std::error_code> modifyEpollEvents(const EndpointContext& context, int fd,
                                                       int epfd, uint32_t new_events,
                                                       uint32_t current_events);

}
