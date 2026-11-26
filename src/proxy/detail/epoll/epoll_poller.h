#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#include <absl/container/flat_hash_map.h>
#include <sys/epoll.h>

#include "common/fd.h"
#include "common/status.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

struct ReadyEvent {
    SourceId source_id;
    uint32_t events;
};

struct EpollOperations {
    void* context = nullptr;
    int (*control)(void*, int epfd, int op, int fd, epoll_event*) = nullptr;
    int (*wait)(void*, int epfd, epoll_event*, int maxevents, int timeout) = nullptr;
};

// Owns an epoll instance and tracks the file descriptors registered with it.
//
// Each watched file descriptor is associated with an application-level SourceId.
// Ready events expose SourceId rather than raw file descriptors to allow the caller to
// resolve event-handler state independently of the poller.
//
// Watched file descriptors are not owned by EpollPoller and must outlive their
// registrations.
class EpollPoller {
public:
    static Result<EpollPoller, std::error_code> create(EpollOperations operations = {});

    // Starts watching fd under SourceId with the given epoll interest mask.
    Status<std::error_code> add(SourceId id, int fd, uint32_t interests);

    // Replaces interests for an existing registration.
    Status<std::error_code> setInterests(SourceId id, uint32_t new_interests);

    // Enables interests for an existing registration.
    Status<std::error_code> enableInterests(SourceId id, uint32_t interests);

    // Disables interests for an existing registration.
    Status<std::error_code> disableInterests(SourceId id, uint32_t interests);

    // Stops watching a source. If kernel deregistration fails, the source
    // remains tracked so that the caller can retry or retire it explicitly.
    Status<std::error_code> remove(SourceId id);

    // Stops tracking a source even if kernel deregistration fails.
    Status<std::error_code> retire(SourceId id);

    // Whether a source is currently being watched.
    bool isWatching(SourceId id) const;

    // The batch is borrowed until the next wait. An interrupted poll returns an empty batch.
    Result<std::span<const ReadyEvent>, std::error_code> wait(bool nonblocking = false);

private:
    static constexpr std::size_t event_buf_cap = 1024;

    struct WatchedFd {
        int fd;
        uint32_t interests;
    };

    EpollPoller(FileDescriptor epfd, EpollOperations operations);

    int control(int op, int fd, epoll_event* event);

    Status<std::error_code> setInterestsImpl(SourceId source_id, uint32_t new_interests,
                                             WatchedFd& watched_fd);

    FileDescriptor epfd_;
    EpollOperations operations_;
    absl::flat_hash_map<SourceId, WatchedFd> watched_;
    std::array<epoll_event, event_buf_cap> event_buffer_{};
    std::array<ReadyEvent, event_buf_cap> ready_events_{};
};

} // namespace orbit::proxy::detail
