#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/status.h"
#include "proxy/detail/session/session_state.h"

namespace orbit::proxy::detail {

struct SendError {
    std::string message;
};

class PendingDataSender {
public:
    static PendingDataSender create(size_t capacity);

    Status<SendError> sendPending(SessionEndpoint& destination);

private:
    explicit PendingDataSender(size_t capacity);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
};

} // namespace orbit::proxy::detail
