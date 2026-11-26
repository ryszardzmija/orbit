#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include <sys/types.h>

namespace orbit::net {

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

// Overrides borrow their context and follow the return value and errno conventions of recv/send.
struct StreamOperations {
    void* context = nullptr;
    ssize_t (*receive)(void*, int fd, void* buffer, size_t size, int flags) = nullptr;
    ssize_t (*send)(void*, int fd, const void* buffer, size_t size, int flags) = nullptr;
};

std::expected<RecvResult, std::error_code> tryRecv(int fd, std::span<uint8_t> buf,
                                                   const StreamOperations& operations);

std::expected<SendResult, std::error_code> trySend(int fd, std::span<const uint8_t> buf,
                                                   const StreamOperations& operations);

} // namespace orbit::net
