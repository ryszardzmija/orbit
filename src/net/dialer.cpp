#include "net/dialer.h"

#include <cerrno>
#include <expected>
#include <format>
#include <system_error>

#include <sys/socket.h>

#include "common/fd.h"
#include "net/address_format.h"
#include "net/resolver.h"

namespace orbit::net {

std::expected<DialSuccess, DialError> dial(const DialSocketAddress& address) {
    auto resolve_result = resolve(address.hostname, address.port, false);
    if (!resolve_result) {
        return std::unexpected(
            DialError{std::format("resolve {}:{} failed: {}", address.hostname, address.port,
                                  resolve_result.error().message)});
    }

    if (resolve_result->empty()) {
        return std::unexpected(DialError{
            std::format("resolve {}:{} returned no addresses", address.hostname, address.port)});
    }

    std::string attempts;
    for (const auto& resolved_address : *resolve_result) {
        std::string addr_str = formatAddress(resolved_address);

        int raw_fd = socket(resolved_address.addr.ss_family, SOCK_STREAM, 0);
        if (raw_fd == -1) {
            if (!attempts.empty()) {
                attempts += " ";
            }
            attempts +=
                std::format("[{} socket: {}]", addr_str, std::system_category().message(errno));
            continue;
        }
        FileDescriptor fd(raw_fd);

        if (int conn_res =
                connect(fd.get(), reinterpret_cast<const sockaddr*>(&resolved_address.addr),
                        resolved_address.addrlen);
            conn_res == -1) {
            if (!attempts.empty()) {
                attempts += " ";
            }
            attempts +=
                std::format("[{} connect: {}]", addr_str, std::system_category().message(errno));
            continue;
        }

        return DialSuccess{std::move(fd), resolved_address};
    }

    return std::unexpected(
        DialError{std::format("dial {}:{} failed: {}", address.hostname, address.port, attempts)});
}

} // namespace orbit::net
