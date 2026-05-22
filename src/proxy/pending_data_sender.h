#pragma once

#include <expected>
#include <memory>
#include <system_error>

#include "proxy/endpoint_context.h"

namespace orbit::proxy {

class PendingDataSender {
public:
    PendingDataSender(size_t capacity, int epfd);

    std::expected<void, std::error_code> sendPending(const EndpointContext& context);

private:
    std::expected<void, std::error_code> setEpollin(const EndpointContext& context);
    std::expected<void, std::error_code> unsetEpollout(const EndpointContext& context);

    std::unique_ptr<uint8_t[]> buf_;
    size_t capacity_;
    int epfd_;
};

} // namespace orbit::proxy
