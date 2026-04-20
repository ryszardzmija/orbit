#pragma once

#include <cstddef>
#include <expected>
#include <system_error>

#include "session_pair.h"

namespace orbit {

class Forwarder {
public:
    Forwarder(size_t capacity, int epfd);

    std::expected<void, std::error_code> forward(SessionEndpoint& endpoint);

private:
    void storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written);
    std::expected<void, std::error_code> unsetEpollin(SessionEndpoint& endpoint);
    std::expected<void, std::error_code> setEpollout(SessionEndpoint& endpoint);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
    int epfd_;
};

} // namespace orbit
