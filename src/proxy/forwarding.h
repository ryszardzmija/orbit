#pragma once

#include <cstddef>
#include <expected>
#include <system_error>

#include "proxy/endpoint_context.h"
#include "proxy/session_pair.h"

namespace orbit {

class Forwarder {
public:
    Forwarder(size_t capacity, int epfd);

    std::expected<void, std::error_code> forward(const EndpointContext& context);

private:
    void storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written);
    std::expected<void, std::error_code> unsetEpollin(const EndpointContext& context);
    std::expected<void, std::error_code> setEpollout(const EndpointContext& context);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
    int epfd_;
};

} // namespace orbit
