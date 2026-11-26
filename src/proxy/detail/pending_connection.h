#pragma once

#include <optional>

#include "common/fd.h"
#include "net/socket_address.h"
#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {

struct PendingConnection {
    FileDescriptor accepted_connection_fd;
    net::SocketAddress accepted_address;
    std::optional<FileDescriptor> attempted_connection_fd;
    // When deferred, the index is the next candidate to try instead of a watched socket.
    size_t attempted_address_idx;
    Deadline deadline;
};

} // namespace orbit::proxy::detail
