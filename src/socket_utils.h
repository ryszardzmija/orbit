#pragma once

#include <expected>
#include <system_error>

#include "connection.h"
#include "fd.h"

namespace orbit {

std::expected<FileDescriptor, std::error_code> createTcpSocket();

std::expected<void, std::error_code> bindAddress(int socket_fd, int port);

std::expected<void, std::error_code> enterListenState(int socket_fd);

std::expected<Connection, std::error_code> acceptClientConnection(int passive_socket_fd);

std::expected<void, std::error_code> setReuseAddressFlag(int socket_fd);

} // namespace orbit
