#include "proxy/forwarding.h"

#include <memory>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "net/socket_io.h"
#include "proxy/epoll_utils.h"
#include "proxy/send_buffer.h"

namespace orbit::proxy {

Forwarder::Forwarder(size_t capacity, int epfd)
    : buf_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity),
      epfd_(epfd) {}

std::expected<void, std::error_code> Forwarder::forward(const EndpointContext& context) {
    auto recv_result =
        net::tryRecv(context.endpoint.socket_fd, std::span<uint8_t>(buf_.get(), capacity_));
    if (!recv_result) {
        return std::unexpected(recv_result.error());
    }

    if (recv_result.value().status == net::RecvStatus::Eof) {
        context.endpoint.done_reading = true;
        if (auto result = unsetEpollin(context); !result) {
            return result;
        }
        return {};
    }

    if (recv_result.value().status == net::RecvStatus::WouldBlock) {
        return {};
    }

    size_t bytes_read = recv_result.value().bytes_received;

    auto send_result = net::trySend(context.endpoint.other->socket_fd,
                                    std::span<const uint8_t>(buf_.get(), bytes_read));

    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    size_t bytes_written = send_result.value().bytes_sent;

    // All the data was successfully sent.
    if (bytes_written == bytes_read) {
        return {};
    }

    storeUnsent(context.endpoint, bytes_read, bytes_written);

    if (context.endpoint.other->send_buffer->status() == SendBuffer::BufferStatus::Paused) {
        if (auto result = unsetEpollin(context); !result) {
            return result;
        }
    }

    if (auto result = setEpollout(context); !result) {
        return result;
    }

    return {};
}

void Forwarder::storeUnsent(SessionEndpoint& endpoint, size_t bytes_read, size_t bytes_written) {
    auto to_buffer =
        std::span<const uint8_t>(buf_.get() + bytes_written, bytes_read - bytes_written);
    endpoint.other->send_buffer->write(to_buffer);
}

std::expected<void, std::error_code> Forwarder::unsetEpollin(const EndpointContext& context) {
    return modifyEpollEvents(context, context.endpoint.socket_fd, epfd_,
                             context.endpoint.current_events & ~EPOLLIN,
                             context.endpoint.current_events);
}

std::expected<void, std::error_code> Forwarder::setEpollout(const EndpointContext& context) {
    EndpointContext other_context = {
        .endpoint = *(context.endpoint.other),
        .endpoint_id = context.other_endpoint_id,
        .other_endpoint_id = context.endpoint_id,
    };

    return modifyEpollEvents(other_context, context.endpoint.other->socket_fd, epfd_,
                             context.endpoint.other->current_events | EPOLLOUT,
                             context.endpoint.other->current_events);
}

} // namespace orbit::proxy
