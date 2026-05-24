#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

// Session state that must currently hold true.
struct SendResult {
    // Source socket is allowed to read more data.
    bool source_reading_allowed;
    // Destination has no pending outbound data.
    bool destination_buffer_drained;
};

struct SendError {
    std::string message;
};

class PendingDataSender {
public:
    static PendingDataSender create(size_t capacity);

    std::expected<SendResult, SendError> sendPending(SessionEndpoint& endpoint);

private:
    explicit PendingDataSender(size_t capacity);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
};

} // namespace orbit::proxy::detail
