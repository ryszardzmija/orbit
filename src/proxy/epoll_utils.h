#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

#include "session_pair.h"

namespace orbit {

std::expected<void, std::error_code> modifyEpollEvents(SessionEndpoint& endpoint, int socket_fd,
                                                       int epfd, uint32_t new_events,
                                                       uint32_t current_events);

}
