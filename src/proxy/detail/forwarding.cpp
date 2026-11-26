#include "proxy/detail/forwarding.h"

#include <algorithm>
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

Status<ForwardError> Forwarder::forward(SessionEndpoint& source, SessionEndpoint& destination,
                                        IdleDeadline* idle,
                                        const net::StreamOperations& operations) {
    assert(destination.state.send_buffer != nullptr);
    auto& queue = *destination.state.send_buffer;
    size_t receive_capacity = std::min(capacity_, queue.remainingCapacity());
    if (receive_capacity == 0) {
        return {};
    }

    auto recv_result =
        net::tryRecv(source.fd.get(), std::span<uint8_t>(buf_.get(), receive_capacity), operations);
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

    std::span<const uint8_t> unsent(buf_.get(), recv_result->bytes_received);
    if (idle && !idle->recordTransfer(unsent.size())) {
        return {};
    }

    // We can try to send the data directly only if the send buffer is empty since otherwise
    // the data would be reordered.
    // Queue residual input even for an empty destination. Its level-triggered writable
    // event continues the drain after this single receive.
    if (!source.state.hungUp() && queue.empty()) {
        if (idle && !idle->canStartIo()) {
            return {};
        }
        auto send_result = net::trySend(destination.fd.get(), unsent, operations);
        if (!send_result) {
            return std::unexpected(ForwardError{
                .message = send_result.error().message(),
                .failed_op = FailedOp::Send,
            });
        }

        size_t bytes_written = send_result.value().bytes_sent;
        if (idle && !idle->recordTransfer(bytes_written)) {
            return {};
        }
        unsent = unsent.subspan(bytes_written);
    }

    if (!queue.write(unsent)) {
        return std::unexpected(ForwardError{
            .message = "Directional send buffer rejected received bytes",
            .failed_op = FailedOp::Send,
        });
    }
    return {};
}

} // namespace orbit::proxy::detail
