#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

namespace orbit {

enum class RecvStatus {
    Ok,
    WouldBlock,
    Eof,
};

enum class SendStatus {
    Ok,
    WouldBlock,
};

struct RecvResult {
    size_t bytes_received;
    RecvStatus status;
};

struct SendResult {
    size_t bytes_sent;
    SendStatus status;
};

std::expected<RecvResult, std::error_code> tryRecv(int fd, std::span<uint8_t> buf);

std::expected<SendResult, std::error_code> trySend(int fd, std::span<const uint8_t> buf);

} // namespace orbit
