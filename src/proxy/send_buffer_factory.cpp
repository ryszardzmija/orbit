#include "send_buffer_factory.h"

#include <memory>

#include "send_buffer.h"

namespace orbit {

SendBufferFactory::SendBufferFactory(const Config& config)
    : block_size_(config.block_size),
      high_watermark_(config.high_watermark),
      low_watermark_(config.low_watermark) {}

std::unique_ptr<SendBuffer> SendBufferFactory::make() const {
    return std::make_unique<SendBuffer>(block_size_, high_watermark_, low_watermark_);
}

} // namespace orbit
