#include "types/date.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace cdb {

TEST(Date, KnownDayNumbers) {
    // Ground truth computed independently (Python's datetime).
    EXPECT_EQ(Date::FromYMD(1970, 1, 1).days, 0);
    EXPECT_EQ(Date::FromYMD(1969, 12, 31).days, -1);
    EXPECT_EQ(Date::FromYMD(2000, 1, 1).days, 10957);
    EXPECT_EQ(Date::FromYMD(2000, 3, 1).days, 11017);
    EXPECT_EQ(Date::FromYMD(1998, 12, 1).days, 10561);
    EXPECT_EQ(Date::FromYMD(1998, 9, 2).days, 10471);
    EXPECT_EQ(Date::FromYMD(2024, 2, 29).days, 19782);
    EXPECT_EQ(Date::FromYMD(1900, 3, 1).days, -25508);
    EXPECT_EQ(Date::FromYMD(1, 1, 1).days, -719162);
    EXPECT_EQ(Date::FromYMD(9999, 12, 31).days, 2932896);
}

TEST(Date, LeapYearRules) {
    EXPECT_TRUE(Date::IsLeapYear(2000));  // divisible by 400
    EXPECT_FALSE(Date::IsLeapYear(1900)); // divisible by 100 but not 400
    EXPECT_TRUE(Date::IsLeapYear(2024));
    EXPECT_FALSE(Date::IsLeapYear(2023));
    EXPECT_FALSE(Date::IsLeapYear(2100));
    EXPECT_TRUE(Date::IsLeapYear(1600));
    EXPECT_EQ(Date::DaysInMonth(2024, 2), 29);
    EXPECT_EQ(Date::DaysInMonth(2023, 2), 28);
    EXPECT_EQ(Date::DaysInMonth(2023, 4), 30);
    EXPECT_EQ(Date::DaysInMonth(2023, 12), 31);
}

// Exhaustive check against the standard library over the entire supported range: every single
// day from 0001-01-01 to 9999-12-31 must convert to the same civil date as std::chrono, and
// back.
TEST(Date, ExhaustiveRoundTripAgainstStdChrono) {
    using namespace std::chrono;
    const int32_t first = Date::FromYMD(1, 1, 1).days;
    const int32_t last = Date::FromYMD(9999, 12, 31).days;
    int32_t prev = first - 1;
    for (int32_t d = first; d <= last; d++) {
        ASSERT_EQ(d, prev + 1);
        prev = d;
        int y, m, day;
        Date::ToYMD(date_t{d}, y, m, day);
        const year_month_day ymd{sys_days{days{d}}};
        ASSERT_EQ(y, static_cast<int>(ymd.year())) << d;
        ASSERT_EQ(static_cast<unsigned>(m), static_cast<unsigned>(ymd.month())) << d;
        ASSERT_EQ(static_cast<unsigned>(day), static_cast<unsigned>(ymd.day())) << d;
        ASSERT_TRUE(Date::IsValid(y, m, day)) << d;
        ASSERT_EQ(Date::FromYMD(y, m, day).days, d) << d;
    }
}

TEST(Date, DayOfWeekMatchesStdChronoExhaustively) {
    using namespace std::chrono;
    for (int32_t d = Date::FromYMD(1, 1, 1).days; d <= Date::FromYMD(9999, 12, 31).days; d += 7) {
        const weekday wd{sys_days{days{d}}};
        ASSERT_EQ(static_cast<unsigned>(Date::DayOfWeek(date_t{d})), wd.c_encoding()) << d;
    }
    EXPECT_EQ(Date::DayOfWeek(Date::FromYMD(1970, 1, 1)), 4);   // Thursday
    EXPECT_EQ(Date::DayOfWeek(Date::FromYMD(2000, 1, 1)), 6);   // Saturday
    EXPECT_EQ(Date::DayOfWeek(Date::FromYMD(1969, 12, 28)), 0); // Sunday, before the epoch
}

TEST(Date, DayOfYear) {
    EXPECT_EQ(Date::DayOfYear(Date::FromYMD(2023, 1, 1)), 1);
    EXPECT_EQ(Date::DayOfYear(Date::FromYMD(2023, 12, 31)), 365);
    EXPECT_EQ(Date::DayOfYear(Date::FromYMD(2024, 12, 31)), 366);
    EXPECT_EQ(Date::DayOfYear(Date::FromYMD(2024, 3, 1)), 61);
}

TEST(Date, Accessors) {
    const date_t d = Date::FromYMD(1998, 9, 2);
    EXPECT_EQ(Date::Year(d), 1998);
    EXPECT_EQ(Date::Month(d), 9);
    EXPECT_EQ(Date::Day(d), 2);
}

TEST(Date, IsValidRejectsNonexistentDates) {
    EXPECT_FALSE(Date::IsValid(2023, 2, 29));
    EXPECT_TRUE(Date::IsValid(2024, 2, 29));
    EXPECT_FALSE(Date::IsValid(2023, 4, 31));
    EXPECT_FALSE(Date::IsValid(2023, 0, 10));
    EXPECT_FALSE(Date::IsValid(2023, 13, 10));
    EXPECT_FALSE(Date::IsValid(2023, 1, 0));
    EXPECT_FALSE(Date::IsValid(2023, 1, 32));
    EXPECT_FALSE(Date::IsValid(0, 1, 1));
    EXPECT_FALSE(Date::IsValid(10000, 1, 1));
    EXPECT_TRUE(Date::IsValid(1, 1, 1));
    EXPECT_TRUE(Date::IsValid(9999, 12, 31));
}

TEST(Date, ToStringFormatsWithZeroPadding) {
    EXPECT_EQ(Date::ToString(Date::FromYMD(1998, 9, 2)), "1998-09-02");
    EXPECT_EQ(Date::ToString(Date::FromYMD(1, 1, 1)), "0001-01-01");
    EXPECT_EQ(Date::ToString(date_t{0}), "1970-01-01");
    EXPECT_EQ(Date::ToString(Date::FromYMD(9999, 12, 31)), "9999-12-31");
}

TEST(Date, FromStringParsesValidInput) {
    EXPECT_EQ(Date::FromString("1970-01-01")->days, 0);
    EXPECT_EQ(Date::FromString("1998-12-01")->days, 10561);
    EXPECT_EQ(Date::FromString("2024-02-29")->days, 19782);
    EXPECT_EQ(Date::FromString("0001-01-01")->days, -719162);
    EXPECT_EQ(Date::FromString("9999-12-31")->days, 2932896);
    // single-digit month/day are accepted
    EXPECT_EQ(Date::FromString("2000-1-5"), Date::FromString("2000-01-05"));
    EXPECT_EQ(Date::FromString("2000-1-5")->days, Date::FromYMD(2000, 1, 5).days);
}

TEST(Date, FromStringRejectsMalformedInput) {
    for (const char* bad : {"",
                            "2020",
                            "2020-",
                            "2020-1",
                            "2020-01",
                            "2020-01-",
                            "2020-13-01",
                            "2020-00-10",
                            "2020-02-30",
                            "2021-02-29",
                            "2023-04-31",
                            "20200101",
                            "2020/01/01",
                            " 2020-01-01",
                            "2020-01-01 ",
                            "2020-01-011",
                            "0000-01-01",
                            "abcd-ef-gh",
                            "2020-1-1x",
                            "+2020-01-01",
                            "2020--1-01",
                            "2020-01--1",
                            "202-01-01",
                            "20201-01-01",
                            "2020-001-01",
                            "2020-01-001",
                            "2020-01-00",
                            "2020-01-32",
                            "-2020-01-01",
                            "2020-0x-01",
                            "2020-01-0x"}) {
        EXPECT_FALSE(Date::FromString(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(Date, StringRoundTripOverTheWholeRange) {
    for (int32_t d = Date::FromYMD(1, 1, 1).days; d <= Date::FromYMD(9999, 12, 31).days; d += 13) {
        const std::string text = Date::ToString(date_t{d});
        ASSERT_EQ(text.size(), 10u);
        auto parsed = Date::FromString(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        ASSERT_EQ(parsed->days, d) << text;
    }
}

TEST(Date, OrderingFollowsDayNumber) {
    EXPECT_LT(Date::FromYMD(1998, 9, 2), Date::FromYMD(1998, 12, 1));
    EXPECT_EQ(Date::FromYMD(2000, 2, 29), Date::FromYMD(2000, 2, 29));
    EXPECT_GT(Date::FromYMD(1970, 1, 1), Date::FromYMD(1969, 12, 31));
}

} // namespace cdb
