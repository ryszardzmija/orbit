#include "proxy/detail/signal_fd.h"

#include <cerrno>
#include <expected>
#include <sys/signalfd.h>
#include <system_error>
#include <unistd.h>

#include "signal/signal_block.h"

namespace orbit::proxy::detail {

std::expected<FileDescriptor, std::error_code> createShutdownSignalFd() {
    auto mask_make_result = makeShutdownSignalMask();
    if (!mask_make_result) {
        return std::unexpected(mask_make_result.error());
    }
    sigset_t mask = mask_make_result.value();

    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return FileDescriptor(fd);
}

} // namespace orbit::proxy::detail
