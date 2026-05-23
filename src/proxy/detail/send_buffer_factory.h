#pragma once

#include <cstdlib>
#include <memory>

#include "proxy/detail/send_buffer.h"
#include "proxy/detail/send_buffer_options.h"

namespace orbit::proxy::detail {

class SendBufferFactory {
public:
    explicit SendBufferFactory(const SendBufferOptions& config);

    std::unique_ptr<SendBuffer> make() const;

private:
    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;
};

} // namespace orbit::proxy::detail
