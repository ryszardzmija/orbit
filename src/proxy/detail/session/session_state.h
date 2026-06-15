#pragma once

#include <memory>

#include "common/fd.h"
#include "proxy/detail/buffer/send_buffer.h"

namespace orbit::proxy::detail {

struct EndpointState {
    std::unique_ptr<SendBuffer> send_buffer;
    bool peer_half_closed;
    bool half_close_sent;
    bool done_reading;
    // Set when the socket reports a hangup (EPOLLHUP): the connection is fully torn down, so we
    // can no longer write to it and any data still buffered for it is undeliverable.
    bool write_closed;
};

struct SessionEndpoint {
    FileDescriptor fd;
    EndpointState state;
};

// Owns resources associated with the session.
struct Session {
    SessionEndpoint downstream;
    SessionEndpoint upstream;
};

} // namespace orbit::proxy::detail
