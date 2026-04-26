#include "socket_options.h"

#include <expected>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>

namespace orbit::net {

std::expected<void, std::error_code> setNonBlocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    if (int result = fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> setReuseAddress(int socket_fd) {
    int opt = 1;
    int result = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

} // namespace orbit::net
