#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/status.h"
#include "proxy/detail/session/session_state.h"

namespace orbit::proxy::detail {

enum class FailedOp {
    Send,
    Recv,
};

// The session is broken and needs to be torn down.
struct ForwardError {
    std::string message;
    FailedOp failed_op;
};

class Forwarder {
public:
    static Forwarder create(size_t capacity);

    Status<ForwardError> forward(SessionEndpoint& source, SessionEndpoint& destination);

    // Reads everything currently available from source into destination's send buffer until the
    // socket would block or reaches EOF. Unlike forward() this ignores the destination's
    // backpressure watermark: it is meant to drain a hung-up source whose data must be preserved
    // before the source is retired, and the amount available is bounded by the kernel buffer.
    Status<ForwardError> drain(SessionEndpoint& source, SessionEndpoint& destination);

private:
    explicit Forwarder(size_t capacity);

    void storeUnsent(SessionEndpoint& destination, size_t bytes_read, size_t bytes_written);
    void bufferData(SessionEndpoint& destination, size_t bytes_read);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
};

} // namespace orbit::proxy::detail
