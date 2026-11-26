#include "proxy/detail/buffer/send_buffer_factory.h"

#include <memory>

#include "proxy/detail/buffer/send_buffer.h"

namespace orbit::proxy::detail {

SendBufferFactory::SendBufferFactory(const SendBufferOptions& config, AllocationChecks allocation,
                                     BufferBudget& budget)
    : block_size_(config.block_size),
      high_watermark_(config.high_watermark),
      low_watermark_(config.low_watermark),
      allocation_(allocation),
      budget_(&budget) {}

std::unique_ptr<SendBuffer> SendBufferFactory::make() const {
    allocation_.check(AllocationPoint::SendBuffer);
    return std::make_unique<SendBuffer>(block_size_, high_watermark_, low_watermark_, allocation_,
                                        budget_);
}

} // namespace orbit::proxy::detail
