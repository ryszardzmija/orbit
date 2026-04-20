#pragma once

#include <cstdlib>
#include <memory>

#include "config.h"
#include "send_buffer.h"

namespace orbit {

class SendBufferFactory {
public:
    explicit SendBufferFactory(const Config& config);

    std::unique_ptr<SendBuffer> make() const;

private:
    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;
};

} // namespace orbit
