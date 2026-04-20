#include "epoll_utils.h"

#include <cerrno>
#include <cstdint>
#include <expected>
#include <system_error>

#include <sys/epoll.h>

#include "session_pair.h"

namespace orbit {

std::expected<void, std::error_code> modifyEpollEvents(SessionEndpoint& endpoint, int socket_fd,
                                                       int epfd, uint32_t new_events,
                                                       uint32_t old_events) {
    endpoint.current_events = new_events;
    epoll_event event = {
        .events = new_events,
        .data = {.ptr = &endpoint},
    };
    if (int result = epoll_ctl(epfd, EPOLL_CTL_MOD, socket_fd, &event); result == -1) {
        endpoint.current_events = old_events;
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

} // namespace orbit
