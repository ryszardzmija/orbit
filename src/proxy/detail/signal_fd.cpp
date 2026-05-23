#include "proxy/detail/signal_fd.h"

#include <cerrno>
#include <expected>
#include <signal.h>
#include <sys/signalfd.h>
#include <system_error>
#include <unistd.h>

namespace orbit::proxy::detail {

namespace {

std::expected<sigset_t, std::error_code> configureSignals() {
    sigset_t mask;

    if (int result = sigemptyset(&mask); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    if (int result = sigaddset(&mask, SIGINT); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    if (int result = sigaddset(&mask, SIGTERM); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    if (int result = sigprocmask(SIG_BLOCK, &mask, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return mask;
}

} // namespace

std::expected<FileDescriptor, std::error_code> createShutdownSignalFd() {
    auto configure_result = configureSignals();
    if (!configure_result) {
        return std::unexpected(configure_result.error());
    }

    sigset_t mask = configure_result.value();

    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

} // namespace orbit::proxy::detail
