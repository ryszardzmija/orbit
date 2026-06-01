#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

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
    static Result<EpollPoller, std::error_code> create();

    // Starts watching fd under source_id with the given epoll interest mask.
    Status<std::error_code> add(SourceId source_id, int fd, uint32_t interests);

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

    // Blocks until at least one watched source becomes ready. Interruptions
    // are retried internally.
    Result<std::vector<ReadyEvent>, std::error_code> wait();

private:
    static constexpr std::size_t event_buf_cap = 1024;

    struct WatchedFd {
        int fd;
        uint32_t interests;
    };

    explicit EpollPoller(FileDescriptor epfd);

    Status<std::error_code> setInterestsImpl(SourceId source_id, uint32_t new_interests,
                                             WatchedFd& watched_fd);

    FileDescriptor epfd_;
    absl::flat_hash_map<SourceId, WatchedFd> watched_;
    std::array<epoll_event, event_buf_cap> event_buffer_;
};

} // namespace orbit::proxy::detail
