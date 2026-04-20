#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

namespace orbit {

enum class RecvStatus {
    Ok,
    Eof,
    NoData,
};

struct RecvResult {
    size_t bytes_received;
    RecvStatus status;
};

std::expected<RecvResult, std::error_code> tryRecv(int fd, std::span<uint8_t> buf);

std::expected<size_t, std::error_code> trySend(int fd, std::span<const uint8_t> buf);

} // namespace orbit
