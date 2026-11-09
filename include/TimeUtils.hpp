#pragma once

#include <chrono>
#include <string>

namespace ipg::timeutil {

/// Formats a time point in local time, e.g. "2026-11-09 14:03:12.345".
[[nodiscard]] std::string formatLocalTimestamp(std::chrono::system_clock::time_point timePoint);

/// Formats a time point as ISO-8601 UTC, e.g. "2026-11-09T11:03:12.345Z".
[[nodiscard]] std::string formatIso8601Utc(std::chrono::system_clock::time_point timePoint);

}  // namespace ipg::timeutil
