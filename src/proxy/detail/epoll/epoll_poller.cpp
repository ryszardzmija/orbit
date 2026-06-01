#include "proxy/detail/epoll/epoll_poller.h"

#include <cassert>
#include <cerrno>
#include <system_error>
#include <utility>

#include <sys/epoll.h>

#include "common/fd.h"
#include "common/status.h"

namespace orbit::proxy::detail {

EpollPoller::EpollPoller(FileDescriptor epfd)
    : epfd_(std::move(epfd)) {}

Result<EpollPoller, std::error_code> EpollPoller::create() {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return EpollPoller(FileDescriptor(epfd));
}

Status<std::error_code> EpollPoller::add(SourceId source_id, int fd, uint32_t interests) {
    auto [it, inserted] = watched_.emplace(source_id, WatchedFd{
                                                          .fd = fd,
                                                          .interests = interests,
                                                      });

    if (!inserted) {
        assert(false && "source ID already registered");
        return std::unexpected(std::make_error_code(std::errc::file_exists));
    }

    epoll_event registration_data = {
        .events = interests,
        .data = {.u64 = source_id},
    };

    if (int result = epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &registration_data); result == -1) {
        auto error = std::error_code(errno, std::system_category());
        watched_.erase(it);
        return std::unexpected(error);
    }

    return {};
}

Status<std::error_code> EpollPoller::setInterests(SourceId source_id, uint32_t new_interests) {
    auto it = watched_.find(source_id);
    if (it == watched_.end()) {
        assert(false && "source ID is not registered");
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    return setInterestsImpl(source_id, new_interests, it->second);
}

Status<std::error_code> EpollPoller::enableInterests(SourceId source_id, uint32_t interests) {
    auto it = watched_.find(source_id);
    if (it == watched_.end()) {
        assert(false && "source ID is not registered");
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    return setInterestsImpl(source_id, it->second.interests | interests, it->second);
}

Status<std::error_code> EpollPoller::disableInterests(SourceId source_id, uint32_t interests) {
    auto it = watched_.find(source_id);
    if (it == watched_.end()) {
        assert(false && "source ID is not registered");
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    return setInterestsImpl(source_id, it->second.interests & ~interests, it->second);
}

Status<std::error_code> EpollPoller::setInterestsImpl(SourceId source_id, uint32_t new_interests,
                                                      WatchedFd& watched_fd) {
    if (watched_fd.interests == new_interests) {
        return {};
    }

    epoll_event registration_data{
        .events = new_interests,
        .data = {.u64 = source_id},
    };

    if (int result = epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, watched_fd.fd, &registration_data);
        result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    watched_fd.interests = new_interests;

    return {};
}

Status<std::error_code> EpollPoller::remove(SourceId id) {
    auto it = watched_.find(id);

    if (it == watched_.end()) {
        assert(false && "source ID is not registered");
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    if (int result = epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, it->second.fd, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    watched_.erase(it);

    return {};
}

Status<std::error_code> EpollPoller::retire(SourceId id) {
    auto it = watched_.find(id);

    if (it == watched_.end()) {
        assert(false && "source ID is not registered");
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    int fd = it->second.fd;
    watched_.erase(it);

    if (int result = epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr); result == -1) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    return {};
}

Result<std::vector<ReadyEvent>, std::error_code> EpollPoller::wait() {
    while (true) {
        int n = epoll_wait(epfd_.get(), event_buffer_.data(),
                           static_cast<int>(event_buffer_.size()), -1);

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        std::vector<ReadyEvent> result;
        result.reserve(n);

        for (int i = 0; i < n; i++) {
            result.push_back({
                .source_id = event_buffer_[i].data.u64,
                .events = event_buffer_[i].events,
            });
        }

        return result;
    }
}

} // namespace orbit::proxy::detail
