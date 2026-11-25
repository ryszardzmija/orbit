#pragma once

#include <optional>
#include <string>

#include "common/fd.h"
#include "net/socket_address.h"
#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {

struct PendingConnection {
    FileDescriptor accepted_connection_fd;
    net::SocketAddress accepted_address;
    std::optional<FileDescriptor> attempted_connection_fd;
    size_t attempted_address_idx;
    Deadline deadline;
    // Any errors that were accumulated when trying to establish a connection with the upstream.
    std::string error_messages;
};

} // namespace orbit::proxy::detail
