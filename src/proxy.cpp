#include "proxy.h"

#include <expected>
#include <system_error>

#include <sys/socket.h>

namespace orbit {

namespace {

std::expected<void, std::error_code> sendAll(int fd, const char* buf, size_t len) {
    size_t bytes_sent = 0;

    while (bytes_sent < len) {
        ssize_t n = send(fd, buf + bytes_sent, len - bytes_sent, MSG_NOSIGNAL);

        if (n == 0) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        if (n == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        bytes_sent += n;
    }

    return {};
}

} // namespace

std::expected<void, std::error_code> runProxy(int downstream_fd, int upstream_fd) {
    constexpr size_t buf_size = 4096;
    char buf[buf_size];

    while (true) {
        ssize_t bytes_read = recv(downstream_fd, buf, sizeof(buf), 0);

        if (bytes_read == 0) {
            return {};
        }

        if (bytes_read == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        if (auto result = sendAll(upstream_fd, buf, static_cast<size_t>(bytes_read)); !result) {
            return result;
        }
    }
}

} // namespace orbit
