#include "proxy/detail/forwarding.h"

#include <cassert>
#include <memory>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "net/socket_io.h"
#include "proxy/detail/send_buffer.h"

namespace orbit::proxy::detail {

Forwarder::Forwarder(size_t capacity)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity) {}

Forwarder Forwarder::create(size_t capacity) {
    assert(capacity > 0);

    return Forwarder(capacity);
}

std::expected<ForwardResult, ForwardError> Forwarder::forward(SessionEndpoint& endpoint) {
    assert(endpoint.other != nullptr);
    assert(endpoint.other->send_buffer != nullptr);
    assert(endpoint.other->send_buffer->status() == SendBuffer::BufferStatus::Accepting);

    auto recv_result = net::tryRecv(endpoint.socket_fd, std::span<uint8_t>(buf_.get(), capacity_));
    if (!recv_result) {
        return std::unexpected(ForwardError{
            .message = recv_result.error().message(),
            .failed_op = FailedOp::Recv,
        });
    }

    if (recv_result.value().status == net::RecvStatus::Eof) {
        endpoint.done_reading = true;

        return ForwardResult{
            .source_reading_allowed = false,
            .destination_has_pending_data = !endpoint.other->send_buffer->empty(),
        };
    }

    if (recv_result.value().status == net::RecvStatus::WouldBlock) {
        return ForwardResult{
            .source_reading_allowed = true,
            .destination_has_pending_data = !endpoint.other->send_buffer->empty(),
        };
    }

    size_t bytes_read = recv_result.value().bytes_received;

    // We can try to send the data directly only if the send buffer is empty since otherwise
    // the data would be reordered.
    if (endpoint.other->send_buffer->empty()) {
        auto send_result =
            net::trySend(endpoint.other->socket_fd, std::span<uint8_t>(buf_.get(), bytes_read));
        if (!send_result) {
            bufferData(endpoint, bytes_read);
            return std::unexpected(ForwardError{
                .message = send_result.error().message(),
                .failed_op = FailedOp::Send,
            });
        }

        size_t bytes_written = send_result.value().bytes_sent;
        if (bytes_written != bytes_read) {
            storeUnsent(endpoint, bytes_read, bytes_written);
        }
    } else {
        bufferData(endpoint, bytes_read);
    }

    if (endpoint.other->send_buffer->status() == SendBuffer::BufferStatus::Paused) {
        return ForwardResult{
            .source_reading_allowed = false,
            .destination_has_pending_data = !endpoint.other->send_buffer->empty(),
        };
    }

    return ForwardResult{
        .source_reading_allowed = true,
        .destination_has_pending_data = !endpoint.other->send_buffer->empty(),
    };
}

void Forwarder::bufferData(SessionEndpoint& endpoint, size_t bytes_read) {
    auto to_buffer = std::span<const uint8_t>(buf_.get(), bytes_read);
    endpoint.other->send_buffer->write(to_buffer);
}

void Forwarder::storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written) {
    auto to_buffer =
        std::span<const uint8_t>(buf_.get() + bytes_written, bytes_read - bytes_written);
    endpoint.other->send_buffer->write(to_buffer);
}

} // namespace orbit::proxy::detail
