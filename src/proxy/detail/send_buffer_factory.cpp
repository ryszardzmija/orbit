#include "proxy/detail/send_buffer_factory.h"

#include <memory>

#include "proxy/detail/send_buffer.h"

namespace orbit::proxy::detail {

SendBufferFactory::SendBufferFactory(const SendBufferOptions& config)
    : block_size_(config.block_size),
      high_watermark_(config.high_watermark),
      low_watermark_(config.low_watermark) {}

std::unique_ptr<SendBuffer> SendBufferFactory::make() const {
    return std::make_unique<SendBuffer>(block_size_, high_watermark_, low_watermark_);
}

} // namespace orbit::proxy::detail
