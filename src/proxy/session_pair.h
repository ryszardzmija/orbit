#pragma once

#include <memory>

#include "send_buffer.h"
#include "send_buffer_factory.h"

namespace orbit {

struct SessionEndpoint {
    int socket_fd;
    SessionEndpoint* other;
    std::unique_ptr<SendBuffer> send_buffer;
    uint32_t current_events; // epoll registration state
    bool peer_half_closed;
    bool half_close_sent;
};

struct SessionPair {
    SessionEndpoint downstream;
    SessionEndpoint upstream;
};

std::unique_ptr<SessionPair> makeSessionPair(int downstream_fd, int upstream_fd,
                                             const SendBufferFactory& factory);

} // namespace orbit
