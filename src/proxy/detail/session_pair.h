#pragma once

#include <memory>

#include "proxy/detail/send_buffer.h"
#include "proxy/detail/send_buffer_factory.h"

namespace orbit::proxy::detail {

struct SessionEndpoint {
    int socket_fd;
    SessionEndpoint* other;
    std::unique_ptr<SendBuffer> send_buffer;
    uint32_t current_events; // epoll registration state
    bool peer_half_closed;
    bool half_close_sent;
    bool done_reading;
};

struct SessionPair {
    SessionEndpoint downstream;
    SessionEndpoint upstream;
};

std::unique_ptr<SessionPair> makeSessionPair(int downstream_fd, int upstream_fd,
                                             uint32_t initial_events,
                                             const SendBufferFactory& factory);

} // namespace orbit::proxy::detail
