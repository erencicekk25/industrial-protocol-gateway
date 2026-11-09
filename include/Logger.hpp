#pragma once

#include <string_view>

namespace ipg {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

/// Minimal thread-safe console logger.
///
/// Every line is written under a single mutex, so output produced by the
/// simulator thread and the gateway thread never interleaves mid-line.
/// Logging never throws: a failure to log must not take the process down.
class Logger {
public:
    Logger() = delete;

    static void setMinimumLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel minimumLevel() noexcept;
    [[nodiscard]] static bool isEnabled(LogLevel level) noexcept;

    static void log(LogLevel level, std::string_view component, std::string_view message) noexcept;

    static void debug(std::string_view component, std::string_view message) noexcept {
        log(LogLevel::Debug, component, message);
    }
    static void info(std::string_view component, std::string_view message) noexcept {
        log(LogLevel::Info, component, message);
    }
    static void warning(std::string_view component, std::string_view message) noexcept {
        log(LogLevel::Warning, component, message);
    }
    static void error(std::string_view component, std::string_view message) noexcept {
        log(LogLevel::Error, component, message);
    }
};

}  // namespace ipg
