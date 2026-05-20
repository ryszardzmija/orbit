#include "net/address_format.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <gtest/gtest.h>

using orbit::net::formatAddress;
using orbit::net::SocketAddress;

namespace {

SocketAddress makeIpv4Address(const std::string& ipAddress, uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    int result = inet_pton(AF_INET, ipAddress.c_str(), &addr.sin_addr);
    if (result < 1) {
        throw std::invalid_argument("inet_pton failed to convert: " + ipAddress);
    }

    SocketAddress socket_addr = {};
    socket_addr.addrlen = sizeof(addr);
    std::memcpy(&socket_addr.addr, &addr, sizeof(addr));

    return socket_addr;
}

SocketAddress makeIpv6Address(const std::string& ipAddress, uint16_t port) {
    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);

    int result = inet_pton(AF_INET6, ipAddress.c_str(), &addr.sin6_addr);
    if (result < 1) {
        throw std::invalid_argument("inet_pton failed to convert: " + ipAddress);
    }

    SocketAddress socket_addr = {};
    socket_addr.addrlen = sizeof(addr);
    std::memcpy(&socket_addr.addr, &addr, sizeof(addr));

    return socket_addr;
}

std::string getFormattedIpv4(const std::string& address, uint16_t port) {
    return address + ":" + std::to_string(port);
}

std::string getFormattedIpv6(const std::string& address, uint16_t port) {
    return "[" + address + "]:" + std::to_string(port);
}

} // namespace

TEST(FormatAddressTest, FormatsValidIpv4Address) {
    const std::string addr_str = "142.250.72.14";
    const uint16_t port = 443;
    SocketAddress addr = makeIpv4Address(addr_str, port);

    std::string formatted_addr = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv4(addr_str, port), formatted_addr);
}

TEST(FormatAddressTest, FormatsValidIpv6Address) {
    const std::string addr_str = "2607:f8b0:4005:802::200e";
    const uint16_t port = 443;
    SocketAddress addr = makeIpv6Address(addr_str, port);

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv6(addr_str, port), formatted_str);
}

TEST(FormatAddressTest, FormatsLoopbackIpv4Address) {
    const std::string addr_str = "127.0.0.1";
    const uint16_t port = 80;
    SocketAddress addr = makeIpv4Address(addr_str, port);

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv4(addr_str, port), formatted_str);
}

TEST(FormatAddressTest, FormatsLoopbackIpv6Address) {
    const std::string addr_str = "::1";
    const uint16_t port = 80;
    SocketAddress addr = makeIpv6Address(addr_str, port);

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv6(addr_str, port), formatted_str);
}

TEST(FormatAddressTest, FormatsWildcardIpv4Address) {
    const std::string addr_str = "0.0.0.0";
    const uint16_t port = 53;
    SocketAddress addr = makeIpv4Address(addr_str, port);

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv4(addr_str, port), formatted_str);
}

TEST(FormatAddressTest, FormatsWildcardIpv6Address) {
    const std::string addr_str = "::";
    const uint16_t port = 53;
    SocketAddress addr = makeIpv6Address(addr_str, port);

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ(getFormattedIpv6(addr_str, port), formatted_str);
}

TEST(FormatAddressTest, ReturnsUnknownAddressForInvalidSocklen) {
    SocketAddress addr = {};
    addr.addr.ss_family = AF_INET;
    addr.addrlen = 0;

    std::string formatted_str = formatAddress(addr);

    EXPECT_EQ("<unknown address>", formatted_str);
}

TEST(FormatAddressTest, ReturnsUnknownAddressForInvalidAddressFamily) {
    const std::string addr_str = "127.0.0.1";
    uint16_t port = 443;
    SocketAddress socket_addr = makeIpv4Address(addr_str, port);
    socket_addr.addr.ss_family = AF_UNSPEC;

    std::string formated_str = formatAddress(socket_addr);

    EXPECT_EQ("<unknown address>", formated_str);
}
