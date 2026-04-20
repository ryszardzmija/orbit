#include "socket_io.h"

#include <sys/socket.h>

namespace orbit {

std::expected<size_t, std::error_code> trySend(int fd, std::span<const uint8_t> buf) {
    ssize_t bytes_sent = send(fd, buf.data(), buf.size(), MSG_NOSIGNAL);

    if (bytes_sent == -1) {
        // Kernel send buffer is full, so 0 bytes were written.
        if (errno == EAGAIN) {
            return 0;
        }

        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return bytes_sent;
}

std::expected<RecvResult, std::error_code> tryRecv(int fd, std::span<uint8_t> buf) {
    ssize_t bytes_received = recv(fd, buf.data(), buf.size(), 0);

    if (bytes_received == -1) {
        // No data in kernel receive buffer.
        if (errno == EAGAIN) {
            return RecvResult{
                .bytes_received = 0,
                .status = RecvStatus::NoData,
            };
        }

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

} // namespace orbit
