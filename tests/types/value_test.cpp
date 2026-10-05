#include "types/value.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <limits>

namespace cdb {

TEST(Value, ConstructAndGetEachType) {
    EXPECT_TRUE(Value::Boolean(true).GetBoolean());
    EXPECT_FALSE(Value::Boolean(false).GetBoolean());
    EXPECT_EQ(Value::Integer(-7).GetInteger(), -7);
    EXPECT_EQ(Value::BigInt(int64_t{1} << 40).GetBigInt(), int64_t{1} << 40);
    EXPECT_EQ(Value::Double(2.5).GetDouble(), 2.5);
    EXPECT_EQ(Value::Date(date_t{10561}).GetDate().days, 10561);
    EXPECT_EQ(Value::Varchar("hi").GetVarchar(), "hi");
    EXPECT_EQ(Value::Integer(1).type(), LogicalType::Integer());
    EXPECT_EQ(Value::Date(date_t{0}).type(), LogicalType::Date());
}

TEST(Value, ExtremeIntegersSurvive) {
    EXPECT_EQ(Value::Integer(std::numeric_limits<int32_t>::min()).GetInteger(),
              std::numeric_limits<int32_t>::min());
    EXPECT_EQ(Value::BigInt(std::numeric_limits<int64_t>::max()).GetBigInt(),
              std::numeric_limits<int64_t>::max());
}

TEST(Value, NullsAreTypedAndNotEqualToValues) {
    for (LogicalType t : test::AllTypes()) {
        Value n = Value::Null(t);
        EXPECT_TRUE(n.IsNull());
        EXPECT_EQ(n.type(), t);
        EXPECT_EQ(n.ToString(), "NULL");
        EXPECT_EQ(n, Value::Null(t)); // structural: NULL == NULL of the same type
    }
    EXPECT_NE(Value::Null(LogicalType::Integer()), Value::Null(LogicalType::BigInt()));
    EXPECT_NE(Value::Null(LogicalType::Integer()), Value::Integer(0));
}

TEST(Value, ToStringByType) {
    EXPECT_EQ(Value::Boolean(true).ToString(), "true");
    EXPECT_EQ(Value::Boolean(false).ToString(), "false");
    EXPECT_EQ(Value::Integer(-42).ToString(), "-42");
    EXPECT_EQ(Value::BigInt(std::numeric_limits<int64_t>::min()).ToString(),
              "-9223372036854775808");
    EXPECT_EQ(Value::Date(date_t{10561}).ToString(), "1998-12-01");
    EXPECT_EQ(Value::Varchar("it's").ToString(), "it's");
    EXPECT_EQ(Value::Varchar("").ToString(), "");
}

TEST(Value, DoubleToStringIsShortestRoundTripWithDecimalPoint) {
    EXPECT_EQ(Value::Double(3.0).ToString(), "3.0");
    EXPECT_EQ(Value::Double(0.0).ToString(), "0.0");
    EXPECT_EQ(Value::Double(-0.0).ToString(), "-0.0");
    EXPECT_EQ(Value::Double(1.5).ToString(), "1.5");
    EXPECT_EQ(Value::Double(0.1).ToString(), "0.1");
    EXPECT_EQ(Value::Double(100.0).ToString(), "100.0");
    EXPECT_EQ(Value::Double(1e20).ToString(), "1e+20");
    EXPECT_EQ(Value::Double(std::numeric_limits<double>::infinity()).ToString(), "inf");
    EXPECT_EQ(Value::Double(-std::numeric_limits<double>::infinity()).ToString(), "-inf");
    EXPECT_EQ(Value::Double(std::numeric_limits<double>::quiet_NaN()).ToString(), "nan");
}

TEST(Value, DoubleToStringRoundTripsExactly) {
    test::Rng rng(7);
    for (int i = 0; i < 2000; i++) {
        const double d = std::uniform_real_distribution<double>(-1e12, 1e12)(rng);
        const std::string text = Value::Double(d).ToString();
        EXPECT_EQ(std::stod(text), d) << text;
    }
}

TEST(Value, CompareIntegersAndDates) {
    EXPECT_LT(Value::Compare(Value::Integer(1), Value::Integer(2)), 0);
    EXPECT_GT(Value::Compare(Value::Integer(2), Value::Integer(1)), 0);
    EXPECT_EQ(Value::Compare(Value::Integer(5), Value::Integer(5)), 0);
    EXPECT_LT(Value::Compare(Value::Integer(std::numeric_limits<int32_t>::min()),
                             Value::Integer(std::numeric_limits<int32_t>::max())),
              0);
    EXPECT_LT(Value::Compare(Value::BigInt(-1), Value::BigInt(0)), 0);
    EXPECT_LT(Value::Compare(Value::Date(date_t{-1}), Value::Date(date_t{0})), 0);
}

TEST(Value, CompareBooleansFalseBeforeTrue) {
    EXPECT_LT(Value::Compare(Value::Boolean(false), Value::Boolean(true)), 0);
    EXPECT_EQ(Value::Compare(Value::Boolean(true), Value::Boolean(true)), 0);
}

TEST(Value, CompareDoublesTotalOrder) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    auto cmp = [](double a, double b) {
        return Value::Compare(Value::Double(a), Value::Double(b));
    };
    EXPECT_EQ(cmp(0.0, -0.0), 0); // signed zeros tie
    EXPECT_EQ(cmp(nan, nan), 0);  // NaN equals NaN, so sort/group are well defined
    EXPECT_GT(cmp(nan, inf), 0);  // NaN sorts after +inf
    EXPECT_LT(cmp(inf, nan), 0);
    EXPECT_GT(cmp(nan, -inf), 0);
    EXPECT_LT(cmp(-inf, inf), 0);
    EXPECT_LT(cmp(-1.5, 2.5), 0);
    EXPECT_LT(cmp(std::numeric_limits<double>::denorm_min(), 1e-300), 0);
}

TEST(Value, CompareStringsBytewiseUnsigned) {
    auto cmp = [](std::string a, std::string b) {
        return Value::Compare(Value::Varchar(std::move(a)), Value::Varchar(std::move(b)));
    };
    EXPECT_LT(cmp("apple", "banana"), 0);
    EXPECT_LT(cmp("app", "apple"), 0);
    EXPECT_EQ(cmp("same", "same"), 0);
    EXPECT_LT(cmp("a", "\xff"), 0); // 0xff sorts after 'a' (unsigned)
    EXPECT_LT(cmp("", "a"), 0);
    EXPECT_LT(cmp(std::string("a\0", 2), std::string("a\0b", 3)), 0);
}

TEST(Value, CompareIsAStrictTotalOrderOnRandomValues) {
    test::Rng rng(99);
    for (LogicalType t : test::AllTypes()) {
        std::vector<Value> vals;
        for (int i = 0; i < 60; i++)
            vals.push_back(test::RandomValue(rng, t, /*null=*/0.0));
        for (const Value& a : vals) {
            EXPECT_EQ(Value::Compare(a, a), 0); // reflexive
            for (const Value& b : vals) {
                const int ab = Value::Compare(a, b), ba = Value::Compare(b, a);
                ASSERT_EQ(ab < 0, ba > 0); // antisymmetric
                ASSERT_EQ(ab == 0, ba == 0);
                for (const Value& c : vals) { // transitive
                    if (Value::Compare(a, b) <= 0 && Value::Compare(b, c) <= 0) {
                        ASSERT_LE(Value::Compare(a, c), 0);
                    }
                }
            }
        }
    }
}

TEST(Value, StructuralEquality) {
    EXPECT_EQ(Value::Integer(3), Value::Integer(3));
    EXPECT_NE(Value::Integer(3), Value::Integer(4));
    EXPECT_NE(Value::Integer(3), Value::BigInt(3)); // different types are never equal
    EXPECT_NE(Value::Integer(3), Value::Date(date_t{3}));
    EXPECT_EQ(Value::Varchar("x"), Value::Varchar("x"));
    EXPECT_EQ(Value::Double(std::numeric_limits<double>::quiet_NaN()),
              Value::Double(std::numeric_limits<double>::quiet_NaN()));
}

TEST(ValueDeathTest, WrongTypeGetterAborts) {
    EXPECT_DEATH(Value::Integer(1).GetBigInt(), "CDB_CHECK");
    EXPECT_DEATH(Value::Null(LogicalType::Integer()).GetInteger(), "CDB_CHECK");
    EXPECT_DEATH(Value::Compare(Value::Integer(1), Value::BigInt(1)), "CDB_CHECK");
    EXPECT_DEATH(Value::Compare(Value::Null(LogicalType::Integer()), Value::Integer(1)),
                 "CDB_CHECK");
}

TEST(Value, DoubleFormattingMatchesPythonReprAndDuckDb) {
    auto text = [](double d) { return Value::Double(d).ToString(); };
    // positional notation for 1e-4 <= |d| < 1e16
    EXPECT_EQ(text(1e15), "1000000000000000.0");
    EXPECT_EQ(text(9999999999999998.0), "9999999999999998.0");
    EXPECT_EQ(text(1e-4), "0.0001");
    EXPECT_EQ(text(0.00012), "0.00012");
    EXPECT_EQ(text(123456789.12345679), "123456789.12345679");
    EXPECT_EQ(text(1234.5), "1234.5");
    EXPECT_EQ(text(-0.25), "-0.25");
    EXPECT_EQ(text(10000000000.0), "10000000000.0");
    // scientific otherwise, with a signed exponent of at least two digits
    EXPECT_EQ(text(1e16), "1e+16");
    EXPECT_EQ(text(1.2345678901234568e17), "1.2345678901234568e+17");
    EXPECT_EQ(text(1e-5), "1e-05");
    EXPECT_EQ(text(1.5e-7), "1.5e-07");
    EXPECT_EQ(text(1e22), "1e+22");
    EXPECT_EQ(text(-1e100), "-1e+100");
    EXPECT_EQ(text(5e-324), "5e-324");
    EXPECT_EQ(text(1.7976931348623157e308), "1.7976931348623157e+308");
    EXPECT_EQ(text(0.0), "0.0");
    EXPECT_EQ(text(-0.0), "-0.0");
    EXPECT_EQ(text(1.0), "1.0");
    EXPECT_EQ(text(100.0), "100.0");
}

TEST(Value, DoubleFormattingRoundTripsEveryBitPattern) {
    test::Rng rng(11);
    size_t checked = 0;
    for (int i = 0; i < 50000; i++) {
        uint64_t bits = rng();
        if (i % 3 == 0)
            bits = (bits & 0x800FFFFFFFFFFFFFULL) |
                   ((uint64_t{1023} + test::RandBelow(rng, 80) - 40) << 52);
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        if (!std::isfinite(d))
            continue;
        const std::string text = Value::Double(d).ToString();
        ASSERT_EQ(std::strtod(text.c_str(), nullptr), d) << text;
        // always unambiguously a floating-point literal
        ASSERT_NE(text.find_first_of(".e"), std::string::npos) << text;
        checked++;
    }
    EXPECT_GT(checked, 40000u);
}

} // namespace cdb
