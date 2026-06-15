#include "proxy/detail/pending_data_sender.h"

#include <cassert>
#include <memory>

#include "net/socket_io.h"

namespace orbit::proxy::detail {

PendingDataSender::PendingDataSender(size_t capacity)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity) {}

PendingDataSender PendingDataSender::create(size_t capacity) {
    assert(capacity > 0);

    return PendingDataSender(capacity);
}

Status<SendError> PendingDataSender::sendPending(SessionEndpoint& destination) {
    assert(destination.state.send_buffer != nullptr);

    size_t bytes_buffered =
        destination.state.send_buffer->copy(std::span<uint8_t>(buf_.get(), capacity_));

    if (bytes_buffered == 0) {
        return {};
    }

    auto send_result =
        net::trySend(destination.fd.get(), std::span<const uint8_t>(buf_.get(), bytes_buffered));
    if (!send_result) {
        return std::unexpected(SendError{send_result.error().message()});
    }

    size_t bytes_written = send_result.value().bytes_sent;
    destination.state.send_buffer->consume(bytes_written);

    return {};
}

} // namespace orbit::proxy::detail
