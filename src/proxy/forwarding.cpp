#include "forwarding.h"

#include <memory>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "epoll_utils.h"
#include "send_buffer.h"
#include "socket_io.h"

namespace orbit {

Forwarder::Forwarder(size_t capacity, int epfd)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity),
      epfd_(epfd) {}

std::expected<void, std::error_code> Forwarder::forward(SessionEndpoint& endpoint) {
    auto recv_result = tryRecv(endpoint.socket_fd, std::span<uint8_t>(buf_.get(), capacity_));
    if (!recv_result) {
        return std::unexpected(recv_result.error());
    }

    if (recv_result.value().status == RecvStatus::Eof) {
        if (auto result = unsetEpollin(endpoint); !result) {
            return result;
        }

        return {};
    }

    if (recv_result.value().status == RecvStatus::NoData) {
        return {};
    }

    size_t bytes_read = recv_result.value().bytes_received;

    auto send_result =
        trySend(endpoint.other->socket_fd, std::span<const uint8_t>(buf_.get(), bytes_read));

    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    size_t bytes_written = send_result.value();

    // All the data was successfully sent.
    if (bytes_written == bytes_read) {
        return {};
    }

    storeUnsent(endpoint, bytes_read, bytes_written);

    if (endpoint.other->send_buffer->status() == SendBuffer::BufferStatus::Paused) {
        if (auto result = unsetEpollin(endpoint); !result) {
            return result;
        }
    }

    if (auto result = setEpollout(endpoint); !result) {
        return result;
    }

    return {};
}

void Forwarder::storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written) {
    auto to_buffer =
        std::span<const uint8_t>(buf_.get() + bytes_written, bytes_read - bytes_written);
    endpoint.other->send_buffer->write(to_buffer);
}

std::expected<void, std::error_code> Forwarder::unsetEpollin(SessionEndpoint& endpoint) {
    return modifyEpollEvents(endpoint, endpoint.socket_fd, epfd_,
                             endpoint.current_events & ~EPOLLIN, endpoint.current_events);
}

std::expected<void, std::error_code> Forwarder::setEpollout(SessionEndpoint& endpoint) {
    return modifyEpollEvents(*endpoint.other, endpoint.other->socket_fd, epfd_,
                             endpoint.other->current_events | EPOLLOUT,
                             endpoint.other->current_events);
}

} // namespace orbit
