#include "proxy/detail/forwarding.h"

#include <cassert>
#include <memory>

#include "net/socket_io.h"
#include "proxy/detail/buffer/send_buffer.h"

namespace orbit::proxy::detail {

Forwarder::Forwarder(size_t capacity)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity) {}

Forwarder Forwarder::create(size_t capacity) {
    assert(capacity > 0);

    return Forwarder(capacity);
}

Status<ForwardError> Forwarder::forward(SessionEndpoint& source, SessionEndpoint& destination) {
    assert(destination.state.send_buffer != nullptr);
    assert(destination.state.send_buffer->status() == SendBuffer::BufferStatus::Accepting);

    auto recv_result = net::tryRecv(source.fd.get(), std::span<uint8_t>(buf_.get(), capacity_));
    if (!recv_result) {
        return std::unexpected(ForwardError{
            .message = recv_result.error().message(),
            .failed_op = FailedOp::Recv,
        });
    }

    if (recv_result.value().status == net::RecvStatus::Eof) {
        source.state.done_reading = true;
        return {};
    }

    if (recv_result.value().status == net::RecvStatus::WouldBlock) {
        return {};
    }

    size_t bytes_read = recv_result.value().bytes_received;

    // We can try to send the data directly only if the send buffer is empty since otherwise
    // the data would be reordered.
    if (destination.state.send_buffer->empty()) {
        auto send_result =
            net::trySend(destination.fd.get(), std::span<uint8_t>(buf_.get(), bytes_read));
        if (!send_result) {
            bufferData(destination, bytes_read);
            return std::unexpected(ForwardError{
                .message = send_result.error().message(),
                .failed_op = FailedOp::Send,
            });
        }

        size_t bytes_written = send_result.value().bytes_sent;
        if (bytes_written != bytes_read) {
            storeUnsent(destination, bytes_read, bytes_written);
        }
    } else {
        bufferData(destination, bytes_read);
    }

    return {};
}

Status<ForwardError> Forwarder::drain(SessionEndpoint& source, SessionEndpoint& destination) {
    while (true) {
        auto recv_result =
            net::tryRecv(source.fd.get(), std::span<uint8_t>(buf_.get(), capacity_));
        if (!recv_result) {
            return std::unexpected(ForwardError{
                .message = recv_result.error().message(),
                .failed_op = FailedOp::Recv,
            });
        }

        if (recv_result.value().status == net::RecvStatus::Eof) {
            source.state.done_reading = true;
            return {};
        }

        if (recv_result.value().status == net::RecvStatus::WouldBlock) {
            return {};
        }

        bufferData(destination, recv_result.value().bytes_received);
    }
}

void Forwarder::bufferData(SessionEndpoint& destination, size_t bytes_read) {
    auto to_buffer = std::span<const uint8_t>(buf_.get(), bytes_read);
    destination.state.send_buffer->write(to_buffer);
}

void Forwarder::storeUnsent(SessionEndpoint& destination, size_t bytes_read, size_t bytes_written) {
    auto to_buffer =
        std::span<const uint8_t>(buf_.get() + bytes_written, bytes_read - bytes_written);
    destination.state.send_buffer->write(to_buffer);
}

} // namespace orbit::proxy::detail
