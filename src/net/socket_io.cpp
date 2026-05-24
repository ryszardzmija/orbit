#include "net/socket_io.h"

#include <sys/socket.h>

namespace orbit::net {

std::expected<SendResult, std::error_code> trySend(int fd, std::span<const uint8_t> buf) {
    while (true) {
        ssize_t bytes_sent = send(fd, buf.data(), buf.size(), MSG_NOSIGNAL);

        if (bytes_sent == -1 && errno == EINTR) {
            continue;
        }

        if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return SendResult{
                .bytes_sent = 0,
                .status = SendStatus::WouldBlock,
            };
        }

        if (bytes_sent == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        return SendResult{
            .bytes_sent = static_cast<size_t>(bytes_sent),
            .status = SendStatus::Ok,
        };
    }
}

std::expected<RecvResult, std::error_code> tryRecv(int fd, std::span<uint8_t> buf) {
    while (true) {
        ssize_t bytes_received = recv(fd, buf.data(), buf.size(), 0);

        if (bytes_received == -1 && errno == EINTR) {
            continue;
        }

        if (bytes_received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return RecvResult{
                .bytes_received = 0,
                .status = RecvStatus::WouldBlock,
            };
        }

        if (bytes_received == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        if (bytes_received == 0) {
            return RecvResult{
                .bytes_received = 0,
                .status = RecvStatus::Eof,
            };
        }

        return RecvResult{
            .bytes_received = static_cast<size_t>(bytes_received),
            .status = RecvStatus::Ok,
        };
    }
}

} // namespace orbit::net
