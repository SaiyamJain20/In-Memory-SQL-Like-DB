#include "types/cast.h"

#include "common/error.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>

namespace cdb {

namespace {

Value V(const char* text) {
    return Value::Varchar(text);
}

bool Fails(const Value& v, LogicalType target) {
    try {
        CastValue(v, target);
    } catch (const Error& e) {
        return e.code() == ErrorCode::Type;
    }
    return false;
}

} // namespace

TEST(Cast, ImplicitCastingIsNumericWideningOnly) {
    const auto I = LogicalType::Integer(), B = LogicalType::BigInt(), D = LogicalType::Double();
    EXPECT_TRUE(CanCastImplicitly(I, I));
    EXPECT_TRUE(CanCastImplicitly(I, B));
    EXPECT_TRUE(CanCastImplicitly(I, D));
    EXPECT_TRUE(CanCastImplicitly(B, D));
    EXPECT_FALSE(CanCastImplicitly(B, I));
    EXPECT_FALSE(CanCastImplicitly(D, B));
    for (LogicalType t : test::AllTypes()) {
        EXPECT_TRUE(CanCastImplicitly(t, t));
        if (t != LogicalType::Varchar()) {
            EXPECT_FALSE(CanCastImplicitly(t, LogicalType::Varchar()));
        }
    }
    EXPECT_FALSE(CanCastImplicitly(LogicalType::Boolean(), I));
    EXPECT_FALSE(CanCastImplicitly(LogicalType::Date(), I));
}

TEST(Cast, CommonSuperType) {
    const auto I = LogicalType::Integer(), B = LogicalType::BigInt(), D = LogicalType::Double();
    EXPECT_EQ(CommonSuperType(I, B), B);
    EXPECT_EQ(CommonSuperType(B, I), B);
    EXPECT_EQ(CommonSuperType(I, D), D);
    EXPECT_EQ(CommonSuperType(D, B), D);
    EXPECT_EQ(CommonSuperType(I, I), I);
    EXPECT_EQ(CommonSuperType(LogicalType::Varchar(), LogicalType::Varchar()),
              LogicalType::Varchar());
    EXPECT_FALSE(CommonSuperType(I, LogicalType::Varchar()).has_value());
    EXPECT_FALSE(CommonSuperType(LogicalType::Date(), I).has_value());
    EXPECT_FALSE(CommonSuperType(LogicalType::Boolean(), I).has_value());
}

TEST(Cast, ExplicitCastabilityMatrixAgreesWithCastValue) {
    // CanCastExplicitly must be exactly the set of type pairs for which CastValue can succeed
    // on *some* value, and CastValue must reject (Error::Type) the others even for non-NULL input.
    test::Rng rng(1);
    for (LogicalType from : test::AllTypes()) {
        for (LogicalType to : test::AllTypes()) {
            const bool allowed = CanCastExplicitly(from, to);
            Value sample = test::RandomValue(rng, from, 0.0);
            if (from.id() == TypeId::Integer)
                sample = Value::Integer(1); // a value that converts everywhere
            if (from.id() == TypeId::BigInt)
                sample = Value::BigInt(1);
            if (from.id() == TypeId::Double)
                sample = Value::Double(1.0);
            if (from.id() == TypeId::Boolean)
                sample = Value::Boolean(true);
            if (from.id() == TypeId::Date)
                sample = Value::Date(date_t{100});
            if (from.id() == TypeId::Varchar)
                sample = Value::Varchar("1");
            if (!allowed) {
                EXPECT_TRUE(Fails(sample, to)) << from.ToString() << " -> " << to.ToString();
            } else if (from.id() != TypeId::Varchar || to.id() == TypeId::Varchar ||
                       to.id() != TypeId::Date) {
                EXPECT_NO_THROW(CastValue(sample, to))
                    << from.ToString() << " -> " << to.ToString();
            }
        }
    }
    EXPECT_FALSE(CanCastExplicitly(LogicalType::Date(), LogicalType::Integer()));
    EXPECT_FALSE(CanCastExplicitly(LogicalType::Integer(), LogicalType::Date()));
    EXPECT_FALSE(CanCastExplicitly(LogicalType::Boolean(), LogicalType::Date()));
    EXPECT_TRUE(CanCastExplicitly(LogicalType::Boolean(), LogicalType::Double()));
    EXPECT_TRUE(CanCastExplicitly(LogicalType::Date(), LogicalType::Varchar()));
}

TEST(Cast, NullStaysNullOfTheTargetType) {
    for (LogicalType from : test::AllTypes()) {
        for (LogicalType to : test::AllTypes()) {
            if (!CanCastExplicitly(from, to))
                continue;
            Value r = CastValue(Value::Null(from), to);
            EXPECT_TRUE(r.IsNull());
            EXPECT_EQ(r.type(), to);
        }
    }
}

TEST(Cast, IdentityReturnsTheSameValue) {
    EXPECT_EQ(CastValue(Value::Integer(5), LogicalType::Integer()), Value::Integer(5));
    EXPECT_EQ(CastValue(Value::Varchar("x"), LogicalType::Varchar()), Value::Varchar("x"));
}

TEST(Cast, DoubleToIntegerRoundsHalfToEvenAndChecksRange) {
    auto to_int = [](double d) {
        return CastValue(Value::Double(d), LogicalType::Integer()).GetInteger();
    };
    EXPECT_EQ(to_int(0.5), 0);
    EXPECT_EQ(to_int(1.5), 2);
    EXPECT_EQ(to_int(2.5), 2);
    EXPECT_EQ(to_int(3.5), 4);
    EXPECT_EQ(to_int(-0.5), 0);
    EXPECT_EQ(to_int(-1.5), -2);
    EXPECT_EQ(to_int(-2.5), -2);
    EXPECT_EQ(to_int(2.4999), 2);
    EXPECT_EQ(to_int(2147483647.0), 2147483647);
    EXPECT_EQ(to_int(-2147483648.0), -2147483648);
    EXPECT_TRUE(Fails(Value::Double(2147483648.0), LogicalType::Integer()));
    EXPECT_TRUE(Fails(Value::Double(-2147483649.0), LogicalType::Integer()));
    EXPECT_TRUE(Fails(Value::Double(std::nan("")), LogicalType::Integer()));
    EXPECT_TRUE(
        Fails(Value::Double(std::numeric_limits<double>::infinity()), LogicalType::BigInt()));
    EXPECT_EQ(CastValue(Value::Double(9.2e18), LogicalType::BigInt()).GetBigInt(),
              9200000000000000000LL);
    EXPECT_TRUE(Fails(Value::Double(9.3e18), LogicalType::BigInt()));
    EXPECT_TRUE(Fails(Value::Double(9223372036854775808.0), LogicalType::BigInt())); // exactly 2^63
    EXPECT_EQ(CastValue(Value::Double(-9223372036854775808.0), LogicalType::BigInt()).GetBigInt(),
              std::numeric_limits<int64_t>::min());
}

TEST(Cast, IntegerNarrowingChecksRange) {
    EXPECT_EQ(CastValue(Value::BigInt(2147483647), LogicalType::Integer()).GetInteger(),
              2147483647);
    EXPECT_TRUE(Fails(Value::BigInt(2147483648LL), LogicalType::Integer()));
    EXPECT_TRUE(Fails(Value::BigInt(-2147483649LL), LogicalType::Integer()));
    EXPECT_EQ(CastValue(Value::Integer(-1), LogicalType::BigInt()).GetBigInt(), -1);
    EXPECT_EQ(CastValue(Value::BigInt(std::numeric_limits<int64_t>::max()), LogicalType::Double())
                  .GetDouble(),
              9223372036854775807.0);
}

TEST(Cast, BooleanConversions) {
    EXPECT_EQ(CastValue(Value::Boolean(true), LogicalType::Integer()).GetInteger(), 1);
    EXPECT_EQ(CastValue(Value::Boolean(false), LogicalType::BigInt()).GetBigInt(), 0);
    EXPECT_EQ(CastValue(Value::Boolean(true), LogicalType::Double()).GetDouble(), 1.0);
    EXPECT_EQ(CastValue(Value::Boolean(true), LogicalType::Varchar()).GetVarchar(), "true");
    EXPECT_TRUE(CastValue(Value::Integer(7), LogicalType::Boolean()).GetBoolean());
    EXPECT_FALSE(CastValue(Value::BigInt(0), LogicalType::Boolean()).GetBoolean());
    EXPECT_TRUE(CastValue(Value::Double(0.1), LogicalType::Boolean()).GetBoolean());
    EXPECT_FALSE(CastValue(Value::Double(0.0), LogicalType::Boolean()).GetBoolean());
    EXPECT_TRUE(CastValue(Value::Double(std::nan("")), LogicalType::Boolean()).GetBoolean());
    for (const char* t : {"true", "TRUE", "T", " t ", "yes", "Y", "1"}) {
        EXPECT_TRUE(CastValue(V(t), LogicalType::Boolean()).GetBoolean()) << t;
    }
    for (const char* f : {"false", "False", "f", "no", "N", "0"}) {
        EXPECT_FALSE(CastValue(V(f), LogicalType::Boolean()).GetBoolean()) << f;
    }
    for (const char* bad : {"", "2", "maybe", "truee", "tru"}) {
        EXPECT_TRUE(Fails(V(bad), LogicalType::Boolean())) << bad;
    }
}

TEST(Cast, StringToIntegerAcceptsDuckDbSpellings) {
    auto to_int = [](const char* s) {
        return CastValue(V(s), LogicalType::Integer()).GetInteger();
    };
    EXPECT_EQ(to_int("0"), 0);
    EXPECT_EQ(to_int("42"), 42);
    EXPECT_EQ(to_int("-17"), -17);
    EXPECT_EQ(to_int("+7"), 7);
    EXPECT_EQ(to_int("  12  "), 12);
    EXPECT_EQ(to_int("\t5\n"), 5);
    EXPECT_EQ(to_int("0x10"), 16);
    EXPECT_EQ(to_int("0xFF"), 255);
    EXPECT_EQ(to_int("-0x10"), -16);
    EXPECT_EQ(to_int("1_000"), 1000);
    EXPECT_EQ(to_int("1_000_000"), 1000000);
    EXPECT_EQ(to_int("1.5"), 2); // via the decimal path, half away from zero
    EXPECT_EQ(to_int("2.5"), 3);
    EXPECT_EQ(to_int("-2.5"), -3);
    EXPECT_EQ(to_int("1e2"), 100);
    EXPECT_EQ(to_int("2147483647"), 2147483647);
    EXPECT_EQ(to_int("-2147483648"), -2147483648);
    for (const char* bad :
         {"", " ", "abc", "12abc", "1__0", "_1", "1_", "0x", "0xG", "--1", "+-1", "1 2",
          "2147483648", "-2147483649", "inf", "nan", "1e999", "9223372036854775808"}) {
        EXPECT_TRUE(Fails(V(bad), LogicalType::Integer())) << "'" << bad << "'";
    }
    EXPECT_EQ(CastValue(V("9223372036854775807"), LogicalType::BigInt()).GetBigInt(),
              std::numeric_limits<int64_t>::max());
    EXPECT_EQ(CastValue(V("-9223372036854775808"), LogicalType::BigInt()).GetBigInt(),
              std::numeric_limits<int64_t>::min());
    EXPECT_TRUE(Fails(V("9223372036854775808"), LogicalType::BigInt()));
}

TEST(Cast, StringToDouble) {
    auto to_d = [](const char* s) { return CastValue(V(s), LogicalType::Double()).GetDouble(); };
    EXPECT_EQ(to_d("1.5"), 1.5);
    EXPECT_EQ(to_d(" -2.5e3 "), -2500.0);
    EXPECT_EQ(to_d(".5"), 0.5);
    EXPECT_EQ(to_d("5."), 5.0);
    EXPECT_EQ(to_d("1_000.5"), 1000.5);
    EXPECT_EQ(to_d("1e-2"), 0.01);
    EXPECT_EQ(to_d("1E+2"), 100.0);
    EXPECT_TRUE(std::isinf(to_d("inf")));
    EXPECT_TRUE(to_d("-Infinity") < 0 && std::isinf(to_d("-Infinity")));
    EXPECT_TRUE(std::isnan(to_d("NaN")));
    EXPECT_TRUE(std::isinf(to_d("1e999"))); // overflow gives infinity, like DuckDB
    for (const char* bad :
         {"", "abc", "1.2.3", "e5", "1e", "1e+", "0x10", "1 2", "--1", "1d", "."}) {
        EXPECT_TRUE(Fails(V(bad), LogicalType::Double())) << "'" << bad << "'";
    }
}

TEST(Cast, StringToDate) {
    EXPECT_EQ(CastValue(V("1998-12-01"), LogicalType::Date()).ToString(), "1998-12-01");
    EXPECT_EQ(CastValue(V(" 1998-12-1 "), LogicalType::Date()).ToString(), "1998-12-01");
    EXPECT_EQ(CastValue(V("infinity"), LogicalType::Date()).ToString(), "9999-12-31");
    EXPECT_EQ(CastValue(V("-Infinity"), LogicalType::Date()).ToString(), "0001-01-01");
    EXPECT_EQ(CastValue(V("INF"), LogicalType::Date()).ToString(), "9999-12-31");
    for (const char* bad : {"", "2020-02-30", "2021-02-29", "1998-13-01", "98-12-01", "1998/12/01",
                            "x", "1998-12-01x"}) {
        EXPECT_TRUE(Fails(V(bad), LogicalType::Date())) << "'" << bad << "'";
    }
}

TEST(Cast, ToVarcharUsesTheCanonicalText) {
    EXPECT_EQ(CastValue(Value::Integer(-5), LogicalType::Varchar()).GetVarchar(), "-5");
    EXPECT_EQ(CastValue(Value::Double(1.0), LogicalType::Varchar()).GetVarchar(), "1.0");
    EXPECT_EQ(CastValue(Value::Double(0.1), LogicalType::Varchar()).GetVarchar(), "0.1");
    EXPECT_EQ(
        CastValue(Value::Date(Date::FromYMD(2020, 1, 5)), LogicalType::Varchar()).GetVarchar(),
        "2020-01-05");
    EXPECT_EQ(CastValue(Value::Boolean(false), LogicalType::Varchar()).GetVarchar(), "false");
}

TEST(Cast, DoubleTextRoundTripsExactlyThroughVarchar) {
    test::Rng rng(2);
    for (int i = 0; i < 20000; i++) {
        uint64_t bits = rng();
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        if (!std::isfinite(d))
            continue;
        const Value back =
            CastValue(CastValue(Value::Double(d), LogicalType::Varchar()), LogicalType::Double());
        ASSERT_EQ(back.GetDouble(), d)
            << CastValue(Value::Double(d), LogicalType::Varchar()).GetVarchar();
    }
    for (int i = 0; i < 5000; i++) {
        const double d = std::uniform_real_distribution<double>(-1e6, 1e6)(rng) *
                         std::pow(10.0, static_cast<int>(test::RandBelow(rng, 30)) - 15);
        const Value back =
            CastValue(CastValue(Value::Double(d), LogicalType::Varchar()), LogicalType::Double());
        ASSERT_EQ(back.GetDouble(), d);
    }
}

TEST(Cast, ErrorsCarryTheTypeCodeAndTheOffendingText) {
    try {
        CastValue(V("abc"), LogicalType::Integer());
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Type);
        EXPECT_NE(std::string(e.what()).find("'abc'"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("INTEGER"), std::string::npos);
    }
    try {
        CastValue(Value::BigInt(5000000000LL), LogicalType::Integer());
        FAIL();
    } catch (const Error& e) {
        EXPECT_NE(std::string(e.what()).find("out of range"), std::string::npos);
    }
    try {
        CastValue(Value::Date(date_t{0}), LogicalType::Integer());
        FAIL();
    } catch (const Error& e) {
        EXPECT_NE(std::string(e.what()).find("Cannot cast DATE to INTEGER"), std::string::npos);
    }
}

// ------------------------------------------------------------------ interval arithmetic

TEST(AddInterval, DaysAndWeeks) {
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(1998, 12, 1), -90, "day")), "1998-09-02");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 2, 28), 2, "day")), "2020-03-01");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 1, 1), 2, "week")), "2020-01-15");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 1, 1), 0, "day")), "2020-01-01");
}

TEST(AddInterval, MonthsClampToTheEndOfTheMonth) {
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 1, 31), 1, "month")), "2020-02-29");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2019, 1, 31), 1, "month")), "2019-02-28");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 3, 31), -1, "month")), "2020-02-29");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 2, 29), 1, "year")), "2021-02-28");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 2, 29), 4, "year")), "2024-02-29");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 12, 15), 1, "month")), "2021-01-15");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 1, 15), -1, "month")), "2019-12-15");
    EXPECT_EQ(Date::ToString(AddInterval(Date::FromYMD(2020, 1, 15), 12, "month")), "2021-01-15");
}

TEST(AddInterval, MonthArithmeticMatchesStdChronoOnRandomInputs) {
    using namespace std::chrono;
    test::Rng rng(3);
    for (int i = 0; i < 20000; i++) {
        const int32_t days =
            static_cast<int32_t>(test::RandBelow(rng, 2000000)) + Date::FromYMD(1000, 1, 1).days;
        const int64_t months = static_cast<int64_t>(test::RandBelow(rng, 2401)) - 1200;
        const year_month_day ymd{sys_days{std::chrono::days{days}}};
        year_month_day moved = ymd + std::chrono::months{months}; // may land on a non-existent day
        year_month_day expect = moved.ok() ? moved : moved.year() / moved.month() / last;
        const date_t got = AddInterval(date_t{days}, months, "month");
        int y, m, d;
        Date::ToYMD(got, y, m, d);
        ASSERT_EQ(y, static_cast<int>(expect.year())) << days << " + " << months;
        ASSERT_EQ(static_cast<unsigned>(m), static_cast<unsigned>(expect.month()));
        ASSERT_EQ(static_cast<unsigned>(d), static_cast<unsigned>(expect.day()));
    }
}

TEST(AddInterval, YearsAreTwelveMonths) {
    test::Rng rng(4);
    for (int i = 0; i < 2000; i++) {
        const date_t d{static_cast<int32_t>(test::RandBelow(rng, 1500000))};
        const int64_t years = static_cast<int64_t>(test::RandBelow(rng, 81)) - 40;
        EXPECT_EQ(AddInterval(d, years, "year"), AddInterval(d, years * 12, "month"));
    }
}

TEST(AddInterval, RangeAndUnitErrors) {
    EXPECT_THROW(AddInterval(Date::FromYMD(9999, 12, 31), 1, "day"), Error);
    EXPECT_THROW(AddInterval(Date::FromYMD(1, 1, 1), -1, "day"), Error);
    EXPECT_THROW(AddInterval(Date::FromYMD(9999, 6, 1), 1, "year"), Error);
    EXPECT_THROW(
        AddInterval(Date::FromYMD(2000, 1, 1), std::numeric_limits<int64_t>::max() / 2, "day"),
        Error);
    try {
        AddInterval(Date::FromYMD(2020, 1, 1), 1, "hour");
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::NotImplemented);
    }
}

} // namespace cdb
