#include "proxy/detail/pending_data_sender.h"

#include <cassert>
#include <memory>

#include <sys/epoll.h>

#include "net/socket_io.h"

namespace orbit::proxy::detail {

PendingDataSender::PendingDataSender(size_t capacity)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity) {}

PendingDataSender PendingDataSender::create(size_t capacity) {
    assert(capacity > 0);

    return PendingDataSender(capacity);
}

std::expected<SendResult, SendError> PendingDataSender::sendPending(SessionEndpoint& endpoint) {
    assert(endpoint.other != nullptr);
    assert(endpoint.send_buffer != nullptr);

    size_t bytes_buffered = endpoint.send_buffer->copy(std::span<uint8_t>(buf_.get(), capacity_));

    if (bytes_buffered == 0) {
        return SendResult{
            .source_reading_allowed =
                endpoint.send_buffer->status() == SendBuffer::BufferStatus::Accepting &&
                !endpoint.other->done_reading,
            .destination_buffer_drained = endpoint.send_buffer->empty(),
        };
    }

    auto send_result =
        net::trySend(endpoint.socket_fd, std::span<const uint8_t>(buf_.get(), bytes_buffered));
    if (!send_result) {
        return std::unexpected(SendError{send_result.error().message()});
    }

    size_t bytes_written = send_result.value().bytes_sent;
    endpoint.send_buffer->consume(bytes_written);

    return SendResult{
        .source_reading_allowed =
            endpoint.send_buffer->status() == SendBuffer::BufferStatus::Accepting &&
            !endpoint.other->done_reading,
        .destination_buffer_drained = endpoint.send_buffer->empty(),
    };
}

} // namespace orbit::proxy::detail
