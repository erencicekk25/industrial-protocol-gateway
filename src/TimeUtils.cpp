#include "TimeUtils.hpp"

#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ipg::timeutil {
namespace {

std::tm toCalendarTime(std::time_t time, bool utc) noexcept {
    std::tm result{};
#ifdef _WIN32
    if (utc) {
        gmtime_s(&result, &time);
    } else {
        localtime_s(&result, &time);
    }
#else
    if (utc) {
        gmtime_r(&time, &result);
    } else {
        localtime_r(&time, &result);
    }
#endif
    return result;
}

std::string format(std::chrono::system_clock::time_point timePoint, bool utc, const char* pattern,
                   const char* suffix) {
    using namespace std::chrono;

    const std::time_t wholeSeconds = system_clock::to_time_t(timePoint);
    long long millis = duration_cast<milliseconds>(timePoint - system_clock::from_time_t(wholeSeconds)).count();
    if (millis < 0 || millis > 999) {
        millis = 0;  // to_time_t may round instead of truncate on some platforms
    }

    const std::tm calendar = toCalendarTime(wholeSeconds, utc);

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::put_time(&calendar, pattern) << '.' << std::setw(3) << std::setfill('0') << millis << suffix;
    return out.str();
}

}  // namespace

std::string formatLocalTimestamp(std::chrono::system_clock::time_point timePoint) {
    return format(timePoint, false, "%Y-%m-%d %H:%M:%S", "");
}

std::string formatIso8601Utc(std::chrono::system_clock::time_point timePoint) {
    return format(timePoint, true, "%Y-%m-%dT%H:%M:%S", "Z");
}

}  // namespace ipg::timeutil
