#pragma once

#include <cstdlib>
#include <memory>

#include "proxy/send_buffer.h"
#include "proxy/send_buffer_options.h"

namespace orbit {

class SendBufferFactory {
public:
    explicit SendBufferFactory(const SendBufferOptions& config);

    std::unique_ptr<SendBuffer> make() const;

private:
    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;
};

} // namespace orbit
