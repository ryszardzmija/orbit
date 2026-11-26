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

Result<ForwardOutcome, ForwardError>
Forwarder::forward(SessionEndpoint& source, SessionEndpoint& destination, IdleDeadline* idle,
                   const net::StreamOperations& operations, bool allow_new_blocks) {
    assert(destination.state.send_buffer != nullptr);
    auto& queue = *destination.state.send_buffer;
    size_t receive_capacity = std::min(capacity_, queue.remainingCapacity());
    if (receive_capacity == 0) {
        return ForwardOutcome::KeepOpen;
    }
    auto reservation = queue.reserveAppend(receive_capacity, allow_new_blocks);
    if (reservation.capacity() == 0) {
        return ForwardOutcome::WaitForCapacity;
    }

    auto recv_result = net::tryRecv(
        source.fd.get(), std::span<uint8_t>(buf_.get(), reservation.capacity()), operations);
    if (!recv_result) {
        return std::unexpected(ForwardError{
            .code = recv_result.error(),
            .failed_op = FailedOp::Recv,
        });
    }

    if (recv_result.value().status == net::RecvStatus::Eof) {
        source.state.done_reading = true;
        return ForwardOutcome::KeepOpen;
    }

    if (recv_result.value().status == net::RecvStatus::WouldBlock) {
        return ForwardOutcome::KeepOpen;
    }

    std::span<const uint8_t> unsent(buf_.get(), recv_result->bytes_received);
    if (idle && !idle->recordTransfer(unsent.size())) {
        return ForwardOutcome::KeepOpen;
    }

    // We can try to send the data directly only if the send buffer is empty since otherwise
    // the data would be reordered.
    // Queue residual input even for an empty destination. Its level-triggered writable
    // event continues the drain after this single receive.
    if (!source.state.hungUp() && queue.empty()) {
        if (idle && !idle->canStartIo()) {
            return ForwardOutcome::KeepOpen;
        }
        auto send_result = net::trySend(destination.fd.get(), unsent, operations);
        if (!send_result) {
            return std::unexpected(ForwardError{
                .code = send_result.error(),
                .failed_op = FailedOp::Send,
            });
        }

        size_t bytes_written = send_result.value().bytes_sent;
        if (idle && !idle->recordTransfer(bytes_written)) {
            return ForwardOutcome::KeepOpen;
        }
        unsent = unsent.subspan(bytes_written);
    }

    try {
        if (queue.write(unsent, reservation)) {
            return ForwardOutcome::KeepOpen;
        }
    } catch (const std::bad_alloc&) {
        // The owner must close because it cannot retain all bytes already read.
        return std::unexpected(ForwardError{
            .code = std::make_error_code(std::errc::not_enough_memory),
            .failed_op = FailedOp::Buffer,
        });
    }
    return std::unexpected(ForwardError{
        .code = std::make_error_code(std::errc::no_buffer_space),
        .failed_op = FailedOp::Buffer,
    });
}

} // namespace orbit::proxy::detail
