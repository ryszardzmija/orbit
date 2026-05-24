#include "proxy/detail/epoll_utils.h"

#include <cerrno>
#include <cstdint>
#include <expected>
#include <system_error>

#include <sys/epoll.h>

#include "proxy/detail/session_pair.h"

namespace orbit::proxy::detail {

std::expected<void, std::error_code> modifyEpollEvents(ReactorSourceId source_id,
                                                       SessionEndpoint& endpoint, int fd, int epfd,
                                                       uint32_t new_events,
                                                       uint32_t current_events) {
    endpoint.current_events = new_events;
    epoll_event event = {
        .events = new_events,
        .data = {.u64 = source_id},
    };

    if (int result = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event); result == -1) {
        endpoint.current_events = current_events;
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

} // namespace orbit::proxy::detail
