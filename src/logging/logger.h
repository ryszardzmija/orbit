#pragma once

#include <spdlog/common.h>

namespace orbit {

class LoggerGuard {
public:
    LoggerGuard() = default;
    ~LoggerGuard();

    LoggerGuard(const LoggerGuard&) = delete;
    LoggerGuard& operator=(const LoggerGuard&) = delete;

    LoggerGuard(LoggerGuard&&) = delete;
    LoggerGuard& operator=(LoggerGuard&&) = delete;
};

[[nodiscard]] LoggerGuard setUpLogger(spdlog::level::level_enum level);

} // namespace orbit
