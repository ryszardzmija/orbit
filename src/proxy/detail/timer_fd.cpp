#include "proxy/detail/timer_fd.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <system_error>
#include <unistd.h>

#include <sys/timerfd.h>

#include "common/fd.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createShutdownTimerFd() {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

std::expected<void, std::error_code> armTimer(int fd, int seconds) {
    assert(seconds > 0);

    itimerspec spec = {};
    spec.it_value.tv_sec = seconds;

    if (int result = timerfd_settime(fd, 0, &spec, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> disarmTimer(int fd) {
    itimerspec spec = {};

    if (int result = timerfd_settime(fd, 0, &spec, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<void, std::error_code> drainTimerFd(int fd) {
    uint64_t expirations = 0;

    while (true) {
        ssize_t n = read(fd, &expirations, sizeof(expirations));

        if (n == sizeof(expirations)) {
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return {};
        }

        if (n == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

} // namespace orbit::proxy::detail
