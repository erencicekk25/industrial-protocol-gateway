#include "Logger.hpp"

#include "TimeUtils.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

namespace ipg {
namespace {

std::atomic<LogLevel> g_minimumLevel{LogLevel::Info};
std::mutex g_outputMutex;

constexpr std::string_view levelLabel(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warning:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?????";
}

}  // namespace

void Logger::setMinimumLevel(LogLevel level) noexcept {
    g_minimumLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::minimumLevel() noexcept {
    return g_minimumLevel.load(std::memory_order_relaxed);
}

bool Logger::isEnabled(LogLevel level) noexcept {
    return static_cast<int>(level) >= static_cast<int>(minimumLevel());
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) noexcept {
    if (!isEnabled(level)) {
        return;
    }
    try {
        const std::string timestamp = timeutil::formatLocalTimestamp(std::chrono::system_clock::now());
        const std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << timestamp << " [" << levelLabel(level) << "] [" << component << "] " << message << '\n'
                  << std::flush;
    } catch (...) {
        // Logging is best effort. Swallowing here keeps noexcept call sites safe.
    }
}

}  // namespace ipg
