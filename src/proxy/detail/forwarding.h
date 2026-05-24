#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

// Session state that must currently hold true.
struct ForwardResult {
    // Source socket is allowed to read more data.
    bool source_reading_allowed;
    // Destination socket has pending outbound data.
    bool destination_has_pending_data;
};

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

    std::expected<ForwardResult, ForwardError> forward(SessionEndpoint& endpoint);

private:
    explicit Forwarder(size_t capacity);

    void storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written);
    void bufferData(SessionEndpoint& endpoint, size_t bytes_read);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
};

} // namespace orbit::proxy::detail
