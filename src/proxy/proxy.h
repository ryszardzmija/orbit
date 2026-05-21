#pragma once

#include <expected>
#include <system_error>

#include "common/fd.h"
#include "proxy/session_pair.h"

namespace orbit {

struct ProxySession {
    FileDescriptor downstream_fd;
    FileDescriptor upstream_fd;
    std::unique_ptr<SessionPair> endpoints;
};

class ProxyReactor {
public:
    static std::expected<ProxyReactor, std::error_code> create(FileDescriptor downstream_fd,
                                                               FileDescriptor upstream_fd);

    [[nodiscard]] std::expected<void, std::error_code> start();

private:
    constexpr static size_t block_size = 4096;
    constexpr static size_t high_watermark = block_size * 64;
    constexpr static size_t low_watermark = block_size * 48;

    constexpr static size_t event_buf_cap = 64;
    constexpr static size_t forwarder_buf_cap = 4096;

    ProxyReactor(FileDescriptor epfd, ProxySession session);

    FileDescriptor epfd_;
    ProxySession session_;
};

} // namespace orbit
