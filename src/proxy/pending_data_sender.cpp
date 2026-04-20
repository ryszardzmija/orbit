#include "pending_data_sender.h"

#include <memory>

#include <sys/epoll.h>

#include "epoll_utils.h"
#include "socket_io.h"

namespace orbit {

PendingDataSender::PendingDataSender(size_t capacity, int epfd)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity),
      epfd_(epfd) {}

std::expected<void, std::error_code> PendingDataSender::sendPending(SessionEndpoint& endpoint) {
    size_t bytes_buffered = endpoint.send_buffer->copy(std::span<uint8_t>(buf_.get(), capacity_));

    if (bytes_buffered == 0) {
        return {};
    }

    auto send_result =
        trySend(endpoint.socket_fd, std::span<const uint8_t>(buf_.get(), bytes_buffered));
    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    size_t bytes_written = send_result.value();
    endpoint.send_buffer->consume(bytes_written);

    if (endpoint.send_buffer->status() == SendBuffer::BufferStatus::Accepting) {
        if (auto result = setEpollin(endpoint); !result) {
            return result;
        }
    }

    if (endpoint.send_buffer->empty()) {
        if (auto result = unsetEpollout(endpoint); !result) {
            return result;
        }
    }
    return {};
}

std::expected<void, std::error_code> PendingDataSender::setEpollin(SessionEndpoint& endpoint) {
    return modifyEpollEvents(*endpoint.other, endpoint.other->socket_fd, epfd_,
                             endpoint.other->current_events | EPOLLIN,
                             endpoint.other->current_events);
}

std::expected<void, std::error_code> PendingDataSender::unsetEpollout(SessionEndpoint& endpoint) {
    return modifyEpollEvents(endpoint, endpoint.socket_fd, epfd_,
                             endpoint.current_events & ~EPOLLOUT, endpoint.current_events);
}

} // namespace orbit
