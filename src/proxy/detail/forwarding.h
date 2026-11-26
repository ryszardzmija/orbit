#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include "common/status.h"
#include "net/socket_io.h"
#include "proxy/detail/session/idle_deadline.h"
#include "proxy/detail/session/session_state.h"

namespace orbit::proxy::detail {

enum class FailedOp {
    Send,
    Recv,
    Buffer,
};

// The session is broken and needs to be torn down.
struct ForwardError {
    std::error_code code;
    FailedOp failed_op;
};

enum class ForwardOutcome { KeepOpen, WaitForCapacity, Interrupted };

class Forwarder {
public:
    static Forwarder create(size_t capacity);

    Result<ForwardOutcome, ForwardError> forward(SessionEndpoint& source,
                                                 SessionEndpoint& destination, IdleDeadline* idle,
                                                 const net::StreamOperations& operations,
                                                 bool allow_new_blocks);

private:
    explicit Forwarder(size_t capacity);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
};

} // namespace orbit::proxy::detail
