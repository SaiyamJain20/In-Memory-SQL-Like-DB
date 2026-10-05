#include "types/date.h"

#include <cstdio>

namespace cdb::Date {

namespace {

// Howard Hinnant's civil-calendar algorithms (public domain), valid for the whole int32 range.
constexpr int32_t DaysFromCivil(int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);                // [0, 399]
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return static_cast<int32_t>(era * 146097 + static_cast<int64_t>(doe) - 719468);
}

constexpr void CivilFromDays(int64_t z, int& y_out, int& m_out, int& d_out) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);                   // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;                 // [1, 12]
    y_out = static_cast<int>(y + (m <= 2));
    m_out = static_cast<int>(m);
    d_out = static_cast<int>(d);
}

} // namespace

int DaysInMonth(int year, int month) noexcept {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }
    return kDays[month - 1];
}

bool IsValid(int year, int month, int day) noexcept {
    return year >= kMinYear && year <= kMaxYear && month >= 1 && month <= 12 && day >= 1 &&
           day <= DaysInMonth(year, month);
}

date_t FromYMD(int year, int month, int day) noexcept {
    return date_t{DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))};
}

void ToYMD(date_t date, int& year, int& month, int& day) noexcept {
    CivilFromDays(date.days, year, month, day);
}

int Year(date_t date) noexcept {
    int y, m, d;
    ToYMD(date, y, m, d);
    return y;
}

int Month(date_t date) noexcept {
    int y, m, d;
    ToYMD(date, y, m, d);
    return m;
}

int Day(date_t date) noexcept {
    int y, m, d;
    ToYMD(date, y, m, d);
    return d;
}

int DayOfWeek(date_t date) noexcept {
    // 1970-01-01 was a Thursday (4).
    const int64_t w = (static_cast<int64_t>(date.days) + 4) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

int DayOfYear(date_t date) noexcept {
    int y, m, d;
    ToYMD(date, y, m, d);
    return date.days - FromYMD(y, 1, 1).days + 1;
}

std::optional<date_t> FromString(std::string_view s) noexcept {
    size_t pos = 0;
    auto read_number = [&](size_t min_digits, size_t max_digits, int& out) -> bool {
        size_t digits = 0;
        int value = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && digits < max_digits) {
            value = value * 10 + (s[pos] - '0');
            pos++;
            digits++;
        }
        if (digits < min_digits) {
            return false;
        }
        out = value;
        return true;
    };

    int year, month, day;
    if (!read_number(4, 4, year))
        return std::nullopt;
    if (pos >= s.size() || s[pos] != '-')
        return std::nullopt;
    pos++;
    if (!read_number(1, 2, month))
        return std::nullopt;
    if (pos >= s.size() || s[pos] != '-')
        return std::nullopt;
    pos++;
    if (!read_number(1, 2, day))
        return std::nullopt;
    if (pos != s.size())
        return std::nullopt;
    if (!IsValid(year, month, day))
        return std::nullopt;
    return FromYMD(year, month, day);
}

std::string ToString(date_t date) {
    int y, m, d;
    ToYMD(date, y, m, d);
    char buf[32];
    if (y >= 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    } else {
        std::snprintf(buf, sizeof(buf), "-%04d-%02d-%02d", -y, m, d);
    }
    return buf;
}

} // namespace cdb::Date
