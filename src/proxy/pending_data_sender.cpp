#include "proxy/pending_data_sender.h"

#include <memory>

#include <sys/epoll.h>

#include "proxy/epoll_utils.h"
#include "proxy/socket_io.h"

namespace orbit {

PendingDataSender::PendingDataSender(size_t capacity, int epfd)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity),
      epfd_(epfd) {}

std::expected<void, std::error_code>
PendingDataSender::sendPending(const EndpointContext& context) {
    size_t bytes_buffered =
        context.endpoint.send_buffer->copy(std::span<uint8_t>(buf_.get(), capacity_));

    if (bytes_buffered == 0) {
        return {};
    }

    auto send_result =
        trySend(context.endpoint.socket_fd, std::span<const uint8_t>(buf_.get(), bytes_buffered));
    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    size_t bytes_written = send_result.value().bytes_sent;
    context.endpoint.send_buffer->consume(bytes_written);

    if (context.endpoint.send_buffer->status() == SendBuffer::BufferStatus::Accepting &&
        !context.endpoint.other->done_reading) {
        if (auto result = setEpollin(context); !result) {
            return result;
        }
    }

    if (context.endpoint.send_buffer->empty()) {
        if (auto result = unsetEpollout(context); !result) {
            return result;
        }
    }
    return {};
}

std::expected<void, std::error_code> PendingDataSender::setEpollin(const EndpointContext& context) {
    EndpointContext other_context = {
        .endpoint = *(context.endpoint.other),
        .endpoint_id = context.other_endpoint_id,
        .other_endpoint_id = context.endpoint_id,
    };

    return modifyEpollEvents(other_context, context.endpoint.other->socket_fd, epfd_,
                             context.endpoint.other->current_events | EPOLLIN,
                             context.endpoint.other->current_events);
}

std::expected<void, std::error_code>
PendingDataSender::unsetEpollout(const EndpointContext& context) {
    return modifyEpollEvents(context, context.endpoint.socket_fd, epfd_,
                             context.endpoint.current_events & ~EPOLLOUT,
                             context.endpoint.current_events);
}

} // namespace orbit
