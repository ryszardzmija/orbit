#include "dialer.h"

#include <cerrno>
#include <expected>
#include <format>
#include <system_error>

#include <sys/socket.h>

#include "address_format.h"
#include "common/fd.h"
#include "resolver.h"

namespace orbit::net {

std::expected<DialSuccess, DialError> dial(const std::string& hostname, uint16_t port) {
    auto resolve_result = resolve(hostname, port, false);
    if (!resolve_result) {
        return std::unexpected(DialError{std::format("resolve {}:{} failed: {}", hostname, port,
                                                     resolve_result.error().message)});
    }

    if (resolve_result->empty()) {
        return std::unexpected(
            DialError{std::format("resolve {}:{} returned no addresses", hostname, port)});
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
        DialError{std::format("dial {}:{} failed: {}", hostname, port, attempts)});
}

} // namespace orbit::net
