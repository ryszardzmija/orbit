#include "logger.h"

#include <cstddef>
#include <memory>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace orbit {

namespace {

constexpr std::size_t log_queue_size = 8192;
constexpr std::size_t log_worker_threads = 1;

} // namespace

LoggerGuard::~LoggerGuard() {
    if (auto dropped = spdlog::thread_pool()->overrun_counter(); dropped > 0) {
        spdlog::warn("dropped {} log messages due to full queue", dropped);
    }
    spdlog::shutdown();
}

LoggerGuard setUpLogger(spdlog::level::level_enum level) {
    spdlog::init_thread_pool(log_queue_size, log_worker_threads);

    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "orbit", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    spdlog::register_logger(logger);
    logger->set_level(level);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::set_default_logger(logger);

    return {};
}

} // namespace orbit
