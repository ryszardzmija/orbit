#include "proxy/detail/timer_fd.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <system_error>
#include <unistd.h>

#include <sys/timerfd.h>

#include "common/fd.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createTimerFd() {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

std::expected<void, std::error_code> setTimer(int fd, std::optional<Deadline> at) {
    itimerspec spec{};
    if (at) {
        // Linux libstdc++ steady_clock and the timerfd both use CLOCK_MONOTONIC.
        // An all-zero it_value disarms the timer, even in absolute mode.
        auto ns =
            std::max(std::chrono::nanoseconds(1),
                     std::chrono::duration_cast<std::chrono::nanoseconds>(at->time_since_epoch()));
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ns);
        spec.it_value.tv_sec = seconds.count();
        spec.it_value.tv_nsec = (ns - seconds).count();
    }
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, nullptr) == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

std::expected<bool, std::error_code> drainTimerFd(int fd) {
    uint64_t expirations = 0;

    while (true) {
        ssize_t n = read(fd, &expirations, sizeof(expirations));

        if (n == sizeof(expirations)) {
            return expirations != 0;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }

        if (n == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

} // namespace orbit::proxy::detail
