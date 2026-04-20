#include "session_pair.h"

#include <memory>

namespace orbit {

std::unique_ptr<SessionPair> makeSessionPair(int downstream_fd, int upstream_fd,
                                             const SendBufferFactory& factory) {
    auto pair = std::make_unique<SessionPair>();

    pair->upstream = {
        .socket_fd = upstream_fd,
        .other = &pair->downstream,
        .send_buffer = factory.make(),
        .current_events = 0,
        .peer_half_closed = false,
        .half_close_sent = false,
        .done_reading = false,
    };

    pair->downstream = {
        .socket_fd = downstream_fd,
        .other = &pair->upstream,
        .send_buffer = factory.make(),
        .current_events = 0,
        .peer_half_closed = false,
        .half_close_sent = false,
        .done_reading = false,
    };

    return pair;
}

} // namespace orbit
