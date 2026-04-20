#pragma once

#include <expected>
#include <memory>
#include <system_error>

#include "session_pair.h"

namespace orbit {

class PendingDataSender {
public:
    PendingDataSender(size_t capacity, int epfd);

    std::expected<void, std::error_code> sendPending(SessionEndpoint& endpoint);

private:
    std::expected<void, std::error_code> setEpollin(SessionEndpoint& endpoint);
    std::expected<void, std::error_code> unsetEpollout(SessionEndpoint& endpoint);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
    int epfd_;
};
} // namespace orbit
