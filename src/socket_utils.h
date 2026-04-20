#pragma once

#include <expected>
#include <system_error>

#include "connection.h"
#include "fd.h"

namespace orbit {

std::expected<FileDescriptor, std::error_code> createBlockingTcpSocket();
std::expected<FileDescriptor, std::error_code> createNonBlockingTcpSocket();

std::expected<void, std::error_code> makeSocketNonBlocking(int socket_fd);

std::expected<void, std::error_code> bindAddress(int socket_fd, int port);

std::expected<void, std::error_code> enterListenState(int socket_fd);

std::expected<Connection, std::error_code> acceptClientConnection(int passive_socket_fd);

std::expected<void, std::error_code> setReuseAddressFlag(int socket_fd);

std::expected<void, std::error_code> connectToRemote(int socket_fd, in_addr_t ipv4_address,
                                                     in_port_t port);

} // namespace orbit
