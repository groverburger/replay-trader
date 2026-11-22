// Calendar arithmetic on naive minutes since 1970-01-01 (no time zones, no libc gmtime/timegm/strptime, which
// differ between macOS, Linux and Windows). Howard Hinnant's days_from_civil / civil_from_days.
#pragma once

#include <cstdint>
#include <cstdio>

inline int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline void civil_from_days(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

inline int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// 0 = Sunday ... 6 = Saturday
inline int weekday_of(int64_t days) { return static_cast<int>(((days % 7) + 11) % 7); }

// "YYYY-MM-DD HH:MM[:SS]" -> minutes; false when it doesn't parse.
inline bool parse_minutes(const char* s, int64_t& out) {
    int y, mo, d, h = 0, mi = 0;
    if (std::sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    out = days_from_civil(y, mo, d) * 1440 + h * 60 + mi;
    return true;
}
