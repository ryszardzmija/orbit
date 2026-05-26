#pragma once

#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "common/fd.h"
#include "net/socket_address.h"

namespace orbit::proxy::detail {

struct PendingConnection {
    FileDescriptor accepted_connection_fd;
    net::SocketAddress accepted_address;
    std::optional<FileDescriptor> attempted_connection_fd;
    size_t attempted_address_idx;
    // Any errors that were accumulated when trying to establish a connection with the upstream.
    std::optional<std::string> error_messages;
    std::optional<std::vector<std::error_code>> error_codes;
};

} // namespace orbit::proxy::detail
