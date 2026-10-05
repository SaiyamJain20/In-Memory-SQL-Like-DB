#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cdb {

// A calendar date as days since 1970-01-01 in the proleptic Gregorian calendar.
struct date_t {
    int32_t days = 0;

    friend constexpr auto operator<=>(const date_t&, const date_t&) = default;
};
static_assert(sizeof(date_t) == 4);

namespace Date {

// Supported range for string parsing / construction: years 1..9999.
inline constexpr int kMinYear = 1;
inline constexpr int kMaxYear = 9999;

constexpr bool IsLeapYear(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) noexcept;

bool IsValid(int year, int month, int day) noexcept;

// Requires IsValid(year, month, day).
date_t FromYMD(int year, int month, int day) noexcept;

void ToYMD(date_t date, int& year, int& month, int& day) noexcept;

int Year(date_t date) noexcept;
int Month(date_t date) noexcept;
int Day(date_t date) noexcept;
int DayOfWeek(date_t date) noexcept; // 0 = Sunday .. 6 = Saturday
int DayOfYear(date_t date) noexcept; // 1..366

// Parses "YYYY-MM-DD" (1-2 digit month/day accepted, surrounding whitespace not). Returns
// nullopt for malformed text or a non-existent date such as 2023-02-29.
std::optional<date_t> FromString(std::string_view text) noexcept;

// "YYYY-MM-DD", year zero-padded to 4 digits.
std::string ToString(date_t date);

} // namespace Date
} // namespace cdb
