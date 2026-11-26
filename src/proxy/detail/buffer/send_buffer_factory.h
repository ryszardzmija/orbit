#pragma once

#include <cstdlib>
#include <memory>

#include "proxy/detail/buffer/send_buffer.h"
#include "proxy/detail/buffer/send_buffer_options.h"

namespace orbit::proxy::detail {

class SendBufferFactory {
public:
    explicit SendBufferFactory(const SendBufferOptions& config, AllocationChecks allocation = {});

    std::unique_ptr<SendBuffer> make() const;

private:
    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;
    AllocationChecks allocation_;
};

} // namespace orbit::proxy::detail
