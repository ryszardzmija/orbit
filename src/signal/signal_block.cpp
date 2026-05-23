#include "signal/signal_block.h"

#include <signal.h>

namespace orbit {

std::expected<sigset_t, std::error_code> makeShutdownSignalMask() {
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

    return mask;
}

std::expected<void, std::error_code> blockSignals(sigset_t mask) {
    if (int result = sigprocmask(SIG_BLOCK, &mask, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

} // namespace orbit
