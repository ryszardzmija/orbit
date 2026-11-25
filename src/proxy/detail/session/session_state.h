#pragma once

#include <memory>

#include "common/fd.h"
#include "proxy/detail/buffer/send_buffer.h"

namespace orbit::proxy::detail {

struct EndpointState {
    // A full hangup releases undeliverable output. The fd can still supply residual input.
    std::unique_ptr<SendBuffer> send_buffer;
    bool peer_half_closed;
    bool half_close_sent;
    bool done_reading;
    bool hungUp() const { return !send_buffer; }
    bool canWrite() const { return !hungUp() && !half_close_sent; }
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
