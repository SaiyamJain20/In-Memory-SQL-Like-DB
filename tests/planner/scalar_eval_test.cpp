#include "planner/scalar_eval.h"

#include "common/error.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace cdb {

namespace {

using E = BoundExprPtr;

E I(int32_t v) {
    return BoundExpr::Constant(Value::Integer(v));
}
E L(int64_t v) {
    return BoundExpr::Constant(Value::BigInt(v));
}
E D(double v) {
    return BoundExpr::Constant(Value::Double(v));
}
E S(std::string v) {
    return BoundExpr::Constant(Value::Varchar(std::move(v)));
}
E Dt(int y, int m, int d) {
    return BoundExpr::Constant(Value::Date(Date::FromYMD(y, m, d)));
}
E B(std::optional<bool> v) {
    return BoundExpr::Constant(v ? Value::Boolean(*v) : Value::Null(LogicalType::Boolean()));
}
E Null(LogicalType t) {
    return BoundExpr::Constant(Value::Null(t));
}

E Bin(OperatorKind op, E l, E r, LogicalType t) {
    return BoundExpr::Binary(op, std::move(l), std::move(r), t);
}
E Fn(FunctionId id, std::vector<E> args, LogicalType t) {
    return BoundExpr::Call(id, std::move(args), t);
}

template <class... Ts> std::vector<E> Vec(Ts&&... a) {
    std::vector<E> v;
    (v.push_back(std::forward<Ts>(a)), ...);
    return v;
}

Value Eval(const BoundExpr& e) {
    return EvaluateConstant(e);
}

bool Throws(const BoundExpr& e, ErrorCode code) {
    try {
        Eval(e);
    } catch (const Error& err) {
        return err.code() == code;
    }
    return false;
}

} // namespace

// ------------------------------------------------------------------ three-valued logic

TEST(ScalarEvalLogic, AndOrNotTruthTables) {
    const std::optional<bool> T = true, F = false, N = std::nullopt;
    const std::optional<bool> vals[] = {T, F, N};
    // reference: Kleene logic
    auto kleene_and = [](std::optional<bool> a, std::optional<bool> b) -> std::optional<bool> {
        if (a == false || b == false)
            return false;
        if (!a || !b)
            return std::nullopt;
        return true;
    };
    auto kleene_or = [](std::optional<bool> a, std::optional<bool> b) -> std::optional<bool> {
        if (a == true || b == true)
            return true;
        if (!a || !b)
            return std::nullopt;
        return false;
    };
    for (auto a : vals) {
        for (auto b : vals) {
            for (bool is_and : {true, false}) {
                Value got = Eval(*Bin(is_and ? OperatorKind::And : OperatorKind::Or, B(a), B(b),
                                      LogicalType::Boolean()));
                auto expect = is_and ? kleene_and(a, b) : kleene_or(a, b);
                ASSERT_EQ(got.IsNull(), !expect.has_value());
                if (expect) {
                    ASSERT_EQ(got.GetBoolean(), *expect);
                }
            }
        }
        Value n = Eval(*BoundExpr::Unary(OperatorKind::Not, B(a), LogicalType::Boolean()));
        ASSERT_EQ(n.IsNull(), !a.has_value());
        if (a) {
            ASSERT_EQ(n.GetBoolean(), !*a);
        }
    }
}

TEST(ScalarEvalLogic, AndOrShortCircuitOnTheLeft) {
    // The right side would overflow, but FALSE AND x / TRUE OR x never evaluate it.
    auto boom = [] {
        return Bin(OperatorKind::Eq,
                   Bin(OperatorKind::Add, I(std::numeric_limits<int32_t>::max()), I(1),
                       LogicalType::Integer()),
                   I(0), LogicalType::Boolean());
    };
    EXPECT_FALSE(
        Eval(*Bin(OperatorKind::And, B(false), boom(), LogicalType::Boolean())).GetBoolean());
    EXPECT_TRUE(Eval(*Bin(OperatorKind::Or, B(true), boom(), LogicalType::Boolean())).GetBoolean());
    EXPECT_TRUE(Throws(*Bin(OperatorKind::And, B(true), boom(), LogicalType::Boolean()),
                       ErrorCode::Execution));
    // NULL on the left does not short-circuit: FALSE on the right still decides the result
    EXPECT_FALSE(Eval(*Bin(OperatorKind::And, B(std::nullopt), B(false), LogicalType::Boolean()))
                     .GetBoolean());
}

TEST(ScalarEvalLogic, IsNullAndComparisonsWithNull) {
    EXPECT_TRUE(Eval(*BoundExpr::IsNull(Null(LogicalType::Integer()), false)).GetBoolean());
    EXPECT_FALSE(Eval(*BoundExpr::IsNull(I(0), false)).GetBoolean());
    EXPECT_TRUE(Eval(*BoundExpr::IsNull(I(0), true)).GetBoolean());
    EXPECT_FALSE(Eval(*BoundExpr::IsNull(Null(LogicalType::Varchar()), true)).GetBoolean());
    for (auto op : {OperatorKind::Eq, OperatorKind::Ne, OperatorKind::Lt, OperatorKind::Ge}) {
        EXPECT_TRUE(
            Eval(*Bin(op, Null(LogicalType::Integer()), I(1), LogicalType::Boolean())).IsNull());
        EXPECT_TRUE(
            Eval(*Bin(op, I(1), Null(LogicalType::Integer()), LogicalType::Boolean())).IsNull());
        EXPECT_TRUE(Eval(*Bin(op, Null(LogicalType::Integer()), Null(LogicalType::Integer()),
                              LogicalType::Boolean()))
                        .IsNull());
    }
}

// ------------------------------------------------------------------ arithmetic

TEST(ScalarEvalArithmetic, Int32OverflowMatchesCompilerBuiltins) {
    test::Rng rng(1);
    const int32_t specials[] = {0,
                                1,
                                -1,
                                2,
                                -2,
                                46340,
                                46341,
                                65536,
                                std::numeric_limits<int32_t>::max(),
                                std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max() - 1};
    std::vector<int32_t> pool(std::begin(specials), std::end(specials));
    for (int i = 0; i < 300; i++)
        pool.push_back(static_cast<int32_t>(rng()));
    struct Op {
        OperatorKind kind;
        bool (*overflows)(int32_t, int32_t, int32_t*);
    };
    const Op ops[] = {
        {OperatorKind::Add,
         [](int32_t a, int32_t b, int32_t* r) { return __builtin_add_overflow(a, b, r); }},
        {OperatorKind::Sub,
         [](int32_t a, int32_t b, int32_t* r) { return __builtin_sub_overflow(a, b, r); }},
        {OperatorKind::Mul,
         [](int32_t a, int32_t b, int32_t* r) { return __builtin_mul_overflow(a, b, r); }},
    };
    for (const Op& op : ops) {
        for (int32_t a : pool) {
            for (int32_t b : pool) {
                int32_t expect;
                const bool overflow = op.overflows(a, b, &expect);
                E e = Bin(op.kind, I(a), I(b), LogicalType::Integer());
                if (overflow) {
                    ASSERT_TRUE(Throws(*e, ErrorCode::Execution))
                        << a << " " << OperatorText(op.kind) << " " << b;
                } else {
                    ASSERT_EQ(Eval(*e).GetInteger(), expect);
                }
            }
        }
    }
}

TEST(ScalarEvalArithmetic, Int64OverflowMatchesCompilerBuiltins) {
    test::Rng rng(2);
    std::vector<int64_t> pool = {0,
                                 1,
                                 -1,
                                 3037000499LL,
                                 3037000500LL,
                                 std::numeric_limits<int64_t>::max(),
                                 std::numeric_limits<int64_t>::min(),
                                 4294967296LL};
    for (int i = 0; i < 200; i++)
        pool.push_back(static_cast<int64_t>(rng()));
    for (int64_t a : pool) {
        for (int64_t b : pool) {
            int64_t r;
            struct {
                OperatorKind k;
                bool o;
                int64_t v;
            } cases[3];
            cases[0] = {OperatorKind::Add, __builtin_add_overflow(a, b, &r), r};
            cases[1] = {OperatorKind::Sub, __builtin_sub_overflow(a, b, &r), r};
            cases[2] = {OperatorKind::Mul, __builtin_mul_overflow(a, b, &r), r};
            for (auto& c : cases) {
                E e = Bin(c.k, L(a), L(b), LogicalType::BigInt());
                if (c.o) {
                    ASSERT_TRUE(Throws(*e, ErrorCode::Execution));
                } else {
                    ASSERT_EQ(Eval(*e).GetBigInt(), c.v);
                }
            }
        }
    }
}

TEST(ScalarEvalArithmetic, ModuloFollowsCButZeroGivesNull) {
    for (int32_t a : {7, -7, 0, 1, std::numeric_limits<int32_t>::min()}) {
        for (int32_t b : {3, -3, 1, 5}) {
            EXPECT_EQ(
                Eval(*Bin(OperatorKind::Mod, I(a), I(b), LogicalType::Integer())).GetInteger(),
                a % b);
        }
        EXPECT_TRUE(Eval(*Bin(OperatorKind::Mod, I(a), I(0), LogicalType::Integer())).IsNull());
    }
    // INT_MIN % -1 would trap in C; the result is 0
    EXPECT_EQ(Eval(*Bin(OperatorKind::Mod, I(std::numeric_limits<int32_t>::min()), I(-1),
                        LogicalType::Integer()))
                  .GetInteger(),
              0);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Mod, L(std::numeric_limits<int64_t>::min()), L(-1),
                        LogicalType::BigInt()))
                  .GetBigInt(),
              0);
    EXPECT_TRUE(Eval(*Bin(OperatorKind::Mod, L(5), L(0), LogicalType::BigInt())).IsNull());
}

TEST(ScalarEvalArithmetic, DoubleIsIeee) {
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(Eval(*Bin(OperatorKind::Div, D(1), D(0), LogicalType::Double())).GetDouble(), inf);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Div, D(-1), D(0), LogicalType::Double())).GetDouble(), -inf);
    EXPECT_TRUE(
        std::isnan(Eval(*Bin(OperatorKind::Div, D(0), D(0), LogicalType::Double())).GetDouble()));
    EXPECT_TRUE(
        std::isnan(Eval(*Bin(OperatorKind::Mod, D(5), D(0), LogicalType::Double())).GetDouble()));
    EXPECT_EQ(Eval(*Bin(OperatorKind::Mod, D(5.5), D(2), LogicalType::Double())).GetDouble(), 1.5);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Mod, D(-5.5), D(2), LogicalType::Double())).GetDouble(),
              -1.5);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Mul, D(1e308), D(10), LogicalType::Double())).GetDouble(),
              inf);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Add, D(0.1), D(0.2), LogicalType::Double())).GetDouble(),
              0.1 + 0.2);
}

TEST(ScalarEvalArithmetic, NegateAndNullPropagation) {
    EXPECT_EQ(
        Eval(*BoundExpr::Unary(OperatorKind::Negate, I(5), LogicalType::Integer())).GetInteger(),
        -5);
    EXPECT_TRUE(
        Throws(*BoundExpr::Unary(OperatorKind::Negate, I(std::numeric_limits<int32_t>::min()),
                                 LogicalType::Integer()),
               ErrorCode::Execution));
    EXPECT_TRUE(
        Throws(*BoundExpr::Unary(OperatorKind::Negate, L(std::numeric_limits<int64_t>::min()),
                                 LogicalType::BigInt()),
               ErrorCode::Execution));
    EXPECT_EQ(
        Eval(*BoundExpr::Unary(OperatorKind::Negate, D(-0.0), LogicalType::Double())).GetDouble(),
        0.0);
    EXPECT_TRUE(Eval(*BoundExpr::Unary(OperatorKind::Negate, Null(LogicalType::Integer()),
                                       LogicalType::Integer()))
                    .IsNull());
    for (auto op : {OperatorKind::Add, OperatorKind::Sub, OperatorKind::Mul, OperatorKind::Mod}) {
        EXPECT_TRUE(
            Eval(*Bin(op, Null(LogicalType::Integer()), I(2), LogicalType::Integer())).IsNull());
        EXPECT_TRUE(
            Eval(*Bin(op, I(2), Null(LogicalType::Integer()), LogicalType::Integer())).IsNull());
    }
}

TEST(ScalarEvalArithmetic, DateArithmetic) {
    EXPECT_EQ(Eval(*Bin(OperatorKind::Add, Dt(2020, 2, 28), I(2), LogicalType::Date())).ToString(),
              "2020-03-01");
    EXPECT_EQ(Eval(*Bin(OperatorKind::Add, I(1), Dt(2019, 12, 31), LogicalType::Date())).ToString(),
              "2020-01-01");
    EXPECT_EQ(Eval(*Bin(OperatorKind::Sub, Dt(2020, 3, 1), I(1), LogicalType::Date())).ToString(),
              "2020-02-29");
    EXPECT_EQ(Eval(*Bin(OperatorKind::Sub, Dt(2020, 3, 1), Dt(2020, 1, 1), LogicalType::BigInt()))
                  .GetBigInt(),
              60);
    EXPECT_EQ(Eval(*Bin(OperatorKind::Sub, Dt(2020, 1, 1), Dt(2020, 3, 1), LogicalType::BigInt()))
                  .GetBigInt(),
              -60);
    EXPECT_TRUE(Throws(*Bin(OperatorKind::Add, Dt(9999, 12, 31), I(1), LogicalType::Date()),
                       ErrorCode::Execution));
    EXPECT_TRUE(Throws(*Bin(OperatorKind::Sub, Dt(1, 1, 1), I(1), LogicalType::Date()),
                       ErrorCode::Execution));
    EXPECT_TRUE(Eval(*Bin(OperatorKind::Add, Dt(2020, 1, 1), Null(LogicalType::Integer()),
                          LogicalType::Date()))
                    .IsNull());
}

TEST(ScalarEvalCompare, TotalOrderForDoublesAndBytewiseStrings) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    auto cmp = [](OperatorKind op, E a, E b) {
        return Eval(*Bin(op, std::move(a), std::move(b), LogicalType::Boolean())).GetBoolean();
    };
    EXPECT_TRUE(cmp(OperatorKind::Eq, D(nan), D(nan)));
    EXPECT_TRUE(cmp(OperatorKind::Gt, D(nan), D(inf)));
    EXPECT_TRUE(cmp(OperatorKind::Lt, D(-inf), D(inf)));
    EXPECT_TRUE(cmp(OperatorKind::Eq, D(0.0), D(-0.0)));
    EXPECT_TRUE(cmp(OperatorKind::Lt, S("a"), S("b")));
    EXPECT_TRUE(cmp(OperatorKind::Lt, S("Z"), S("a"))); // bytewise, case sensitive
    EXPECT_TRUE(cmp(OperatorKind::Lt, S("abc"), S("abcd")));
    EXPECT_TRUE(cmp(OperatorKind::Gt, S("\xff"), S("a")));
    EXPECT_TRUE(cmp(OperatorKind::Ne, I(1), I(2)));
    EXPECT_TRUE(cmp(OperatorKind::Le, Dt(2020, 1, 1), Dt(2020, 1, 1)));
    EXPECT_TRUE(cmp(OperatorKind::Ge, L(5), L(5)));
}

// ------------------------------------------------------------------ strings

namespace {

// Independent reference for LIKE: dynamic programming over code points.
std::u32string U32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char32_t cp = n == 1 ? c : (c & (0xFF >> (n + 1)));
        for (size_t k = 1; k < n; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

bool ReferenceLike(const std::string& text, const std::string& pattern) {
    const std::u32string t = U32(text), p = U32(pattern);
    std::vector<std::vector<bool>> dp(t.size() + 1, std::vector<bool>(p.size() + 1, false));
    dp[0][0] = true;
    for (size_t j = 1; j <= p.size(); j++)
        dp[0][j] = dp[0][j - 1] && p[j - 1] == U'%';
    for (size_t i = 1; i <= t.size(); i++) {
        for (size_t j = 1; j <= p.size(); j++) {
            if (p[j - 1] == U'%')
                dp[i][j] = dp[i][j - 1] || dp[i - 1][j];
            else
                dp[i][j] = dp[i - 1][j - 1] && (p[j - 1] == U'_' || p[j - 1] == t[i - 1]);
        }
    }
    return dp[t.size()][p.size()];
}

} // namespace

TEST(ScalarEvalString, LikeAgainstAnIndependentReferenceMatcher) {
    test::Rng rng(3);
    const std::vector<std::string> text_alpha = {"a", "b", "c", "\xc3\xa9", "\xe6\x97\xa5",
                                                 "%", "_", " "};
    const std::vector<std::string> pat_alpha = {"a", "b", "c", "\xc3\xa9", "\xe6\x97\xa5",
                                                "%", "%", "_", "_"};
    size_t matches = 0, misses = 0;
    for (int i = 0; i < 30000; i++) {
        std::string text, pattern;
        for (size_t n = test::RandBelow(rng, 9); n > 0; n--)
            text += text_alpha[test::RandBelow(rng, text_alpha.size())];
        for (size_t n = test::RandBelow(rng, 7); n > 0; n--)
            pattern += pat_alpha[test::RandBelow(rng, pat_alpha.size())];
        const bool expect = ReferenceLike(text, pattern);
        ASSERT_EQ(LikeMatch(text, pattern), expect)
            << "text='" << text << "' pattern='" << pattern << "'";
        (expect ? matches : misses)++;
    }
    EXPECT_GT(matches, 3000u);
    EXPECT_GT(misses, 3000u);
}

TEST(ScalarEvalString, LikeSpecialCases) {
    EXPECT_TRUE(LikeMatch("", ""));
    EXPECT_TRUE(LikeMatch("", "%"));
    EXPECT_TRUE(LikeMatch("", "%%%"));
    EXPECT_FALSE(LikeMatch("", "_"));
    EXPECT_FALSE(LikeMatch("a", ""));
    EXPECT_TRUE(LikeMatch("a%b", "a%b"));  // '%' in the text is just a character
    EXPECT_FALSE(LikeMatch("ABC", "abc")); // case sensitive
    EXPECT_TRUE(LikeMatch("special requests", "%special%requests%"));
    EXPECT_FALSE(LikeMatch("requests special", "%special%requests%"));
    EXPECT_TRUE(LikeMatch(std::string(10000, 'a') + "b", "%a%b")); // no pathological backtracking
    EXPECT_FALSE(LikeMatch(std::string(5000, 'a'), std::string(2000, '%') + "b"));
    EXPECT_FALSE(LikeMatch("backslash\\", "backslash\\\\")); // there is no escape character
}

TEST(ScalarEvalString, ConcatLengthCaseAndSubstring) {
    EXPECT_EQ(
        Eval(*Bin(OperatorKind::Concat, S("ab"), S("cd"), LogicalType::Varchar())).GetVarchar(),
        "abcd");
    EXPECT_TRUE(Eval(*Bin(OperatorKind::Concat, S("ab"), Null(LogicalType::Varchar()),
                          LogicalType::Varchar()))
                    .IsNull());
    EXPECT_EQ(
        Eval(*Fn(FunctionId::Length, Vec(S("h\xc3\xa9llo")), LogicalType::BigInt())).GetBigInt(),
        5);
    EXPECT_EQ(
        Eval(*Fn(FunctionId::Length, Vec(S("\xe6\x97\xa5\xe6\x9c\xac")), LogicalType::BigInt()))
            .GetBigInt(),
        2);
    EXPECT_EQ(
        Eval(*Fn(FunctionId::Upper, Vec(S("aBc1-\xc3\xa9")), LogicalType::Varchar())).GetVarchar(),
        "ABC1-\xc3\xa9");
    EXPECT_EQ(Eval(*Fn(FunctionId::Lower, Vec(S("aBc1-")), LogicalType::Varchar())).GetVarchar(),
              "abc1-");
    auto sub = [](const char* s, int64_t a, std::optional<int64_t> n) {
        std::vector<E> args = Vec(S(s), L(a));
        if (n)
            args.push_back(L(*n));
        return Eval(*Fn(FunctionId::Substring, std::move(args), LogicalType::Varchar()))
            .GetVarchar();
    };
    EXPECT_EQ(sub("hello", 2, 3), "ell");
    EXPECT_EQ(sub("hello", 1, 5), "hello");
    EXPECT_EQ(sub("hello", 1, 100), "hello");
    EXPECT_EQ(sub("hello", 6, 1), "");
    EXPECT_EQ(sub("hello", 0, 2), "h");
    EXPECT_EQ(sub("hello", -1, 3), "o");
    EXPECT_EQ(sub("hello", 3, std::nullopt), "llo");
    EXPECT_EQ(sub("h\xc3\xa9llo", 2, 2), "\xc3\xa9l"); // counts characters, not bytes
}

TEST(ScalarEvalString, SubstringPrefixProperty) {
    test::Rng rng(4);
    for (int i = 0; i < 2000; i++) {
        std::string s;
        for (size_t n = test::RandBelow(rng, 12); n > 0; n--)
            s +=
                test::Chance(rng, 0.3) ? "\xc3\xa9" : std::string(1, 'a' + test::RandBelow(rng, 5));
        const std::u32string u = U32(s);
        const int64_t k = static_cast<int64_t>(test::RandBelow(rng, 15));
        const std::string got =
            Eval(*Fn(FunctionId::Substring, Vec(S(s), L(1), L(k)), LogicalType::Varchar()))
                .GetVarchar();
        EXPECT_EQ(U32(got).size(), std::min<size_t>(u.size(), static_cast<size_t>(k)));
        EXPECT_EQ(U32(got), u.substr(0, static_cast<size_t>(k)));
    }
}

// ------------------------------------------------------------------ functions

TEST(ScalarEvalFunction, NumericFunctions) {
    EXPECT_EQ(Eval(*Fn(FunctionId::Abs, Vec(I(-5)), LogicalType::Integer())).GetInteger(), 5);
    EXPECT_EQ(Eval(*Fn(FunctionId::Abs, Vec(D(-2.5)), LogicalType::Double())).GetDouble(), 2.5);
    EXPECT_TRUE(Throws(
        *Fn(FunctionId::Abs, Vec(I(std::numeric_limits<int32_t>::min())), LogicalType::Integer()),
        ErrorCode::Execution));
    EXPECT_TRUE(Throws(
        *Fn(FunctionId::Abs, Vec(L(std::numeric_limits<int64_t>::min())), LogicalType::BigInt()),
        ErrorCode::Execution));
    auto round = [](double x, std::optional<int64_t> d) {
        std::vector<E> args = Vec(D(x));
        if (d)
            args.push_back(L(*d));
        return Eval(*Fn(FunctionId::Round, std::move(args), LogicalType::Double())).GetDouble();
    };
    EXPECT_EQ(round(2.5, std::nullopt), 3.0); // half away from zero
    EXPECT_EQ(round(-2.5, std::nullopt), -3.0);
    EXPECT_EQ(round(2.567, 2), 2.57);
    EXPECT_EQ(round(1234.5678, -2), 1200.0);
    EXPECT_EQ(round(1234.5678, 1), 1234.6);
    auto round_int = [](int64_t x, int64_t d) {
        return Eval(*Fn(FunctionId::Round, Vec(L(x), L(d)), LogicalType::BigInt())).GetBigInt();
    };
    EXPECT_EQ(round_int(1250, -2), 1300);
    EXPECT_EQ(round_int(-1250, -2), -1300);
    EXPECT_EQ(round_int(1249, -2), 1200);
    EXPECT_EQ(round_int(1234, 0), 1234);
    EXPECT_EQ(round_int(1234, 3), 1234);
    EXPECT_EQ(round_int(1234, -10), 0);
    EXPECT_EQ(Eval(*Fn(FunctionId::Floor, Vec(D(-1.5)), LogicalType::Double())).GetDouble(), -2.0);
    EXPECT_EQ(Eval(*Fn(FunctionId::Ceil, Vec(D(-1.5)), LogicalType::Double())).GetDouble(), -1.0);
}

TEST(ScalarEvalFunction, DatePartsMatchTheDateLibraryOnRandomDates) {
    test::Rng rng(5);
    for (int i = 0; i < 3000; i++) {
        const date_t d{static_cast<int32_t>(test::RandBelow(rng, 3000000)) - 719162};
        E c = BoundExpr::Constant(Value::Date(d));
        auto part = [&](FunctionId id) {
            return Eval(*Fn(id, Vec(c->Clone()), LogicalType::BigInt())).GetBigInt();
        };
        EXPECT_EQ(part(FunctionId::Year), Date::Year(d));
        EXPECT_EQ(part(FunctionId::Month), Date::Month(d));
        EXPECT_EQ(part(FunctionId::Day), Date::Day(d));
        EXPECT_EQ(part(FunctionId::DayOfWeek), Date::DayOfWeek(d));
        EXPECT_EQ(part(FunctionId::DayOfYear), Date::DayOfYear(d));
        EXPECT_EQ(part(FunctionId::Quarter), (Date::Month(d) - 1) / 3 + 1);
    }
}

TEST(ScalarEvalFunction, StrictFunctionsPropagateNull) {
    for (FunctionId id : {FunctionId::Year, FunctionId::Month, FunctionId::Day}) {
        EXPECT_TRUE(Eval(*Fn(id, Vec(Null(LogicalType::Date())), LogicalType::BigInt())).IsNull());
    }
    EXPECT_TRUE(
        Eval(*Fn(FunctionId::Upper, Vec(Null(LogicalType::Varchar())), LogicalType::Varchar()))
            .IsNull());
    EXPECT_TRUE(Eval(*Fn(FunctionId::Substring, Vec(S("abc"), Null(LogicalType::BigInt())),
                         LogicalType::Varchar()))
                    .IsNull());
    EXPECT_TRUE(Eval(*Fn(FunctionId::Like, Vec(S("abc"), Null(LogicalType::Varchar())),
                         LogicalType::Boolean()))
                    .IsNull());
}

TEST(ScalarEvalFunction, CoalesceAndNullIf) {
    EXPECT_EQ(Eval(*Fn(FunctionId::Coalesce, Vec(Null(LogicalType::Integer()), I(2), I(3)),
                       LogicalType::Integer()))
                  .GetInteger(),
              2);
    EXPECT_TRUE(Eval(*Fn(FunctionId::Coalesce,
                         Vec(Null(LogicalType::Integer()), Null(LogicalType::Integer())),
                         LogicalType::Integer()))
                    .IsNull());
    // later arguments are not evaluated once a value is found
    EXPECT_EQ(Eval(*Fn(FunctionId::Coalesce,
                       Vec(I(1), Bin(OperatorKind::Add, I(std::numeric_limits<int32_t>::max()),
                                     I(1), LogicalType::Integer())),
                       LogicalType::Integer()))
                  .GetInteger(),
              1);
    EXPECT_TRUE(Eval(*Fn(FunctionId::NullIf, Vec(I(1), I(1)), LogicalType::Integer())).IsNull());
    EXPECT_EQ(Eval(*Fn(FunctionId::NullIf, Vec(I(1), I(2)), LogicalType::Integer())).GetInteger(),
              1);
    EXPECT_EQ(Eval(*Fn(FunctionId::NullIf, Vec(I(1), Null(LogicalType::Integer())),
                       LogicalType::Integer()))
                  .GetInteger(),
              1);
    EXPECT_TRUE(Eval(*Fn(FunctionId::NullIf, Vec(Null(LogicalType::Integer()), I(1)),
                         LogicalType::Integer()))
                    .IsNull());
}

// ------------------------------------------------------------------ CASE / IN / CAST / refs

TEST(ScalarEvalMisc, CaseEvaluatesLazilyAndSkipsNullConditions) {
    // WHEN NULL does not match; the first TRUE branch wins; later branches are not evaluated
    EXPECT_EQ(Eval(*BoundExpr::Case(Vec(B(std::nullopt), I(1), B(true), I(2), B(true), I(3), I(4)),
                                    LogicalType::Integer()))
                  .GetInteger(),
              2);
    EXPECT_EQ(
        Eval(*BoundExpr::Case(Vec(B(false), I(1), I(9)), LogicalType::Integer())).GetInteger(), 9);
    E lazy = BoundExpr::Case(Vec(B(true), I(1),
                                 Bin(OperatorKind::Add, I(std::numeric_limits<int32_t>::max()),
                                     I(1), LogicalType::Integer())),
                             LogicalType::Integer());
    EXPECT_EQ(Eval(*lazy).GetInteger(), 1);
}

TEST(ScalarEvalMisc, InListThreeValuedSemantics) {
    auto in = [](E v, std::vector<E> items, bool negated) {
        return Eval(*BoundExpr::InList(std::move(v), std::move(items), negated));
    };
    EXPECT_TRUE(in(I(1), Vec(I(1), I(2)), false).GetBoolean());
    EXPECT_FALSE(in(I(3), Vec(I(1), I(2)), false).GetBoolean());
    EXPECT_TRUE(in(I(3), Vec(I(1), I(2)), true).GetBoolean());
    EXPECT_TRUE(in(I(3), Vec(I(1), Null(LogicalType::Integer())), false).IsNull());
    EXPECT_TRUE(in(I(1), Vec(I(1), Null(LogicalType::Integer())), false)
                    .GetBoolean()); // found despite NULL
    EXPECT_FALSE(in(I(1), Vec(I(1), Null(LogicalType::Integer())), true).GetBoolean());
    EXPECT_TRUE(in(I(3), Vec(I(1), Null(LogicalType::Integer())), true).IsNull());
    EXPECT_TRUE(in(Null(LogicalType::Integer()), Vec(I(1)), false).IsNull());
    EXPECT_TRUE(in(S("b"), Vec(S("a"), S("b")), false).GetBoolean());
}

TEST(ScalarEvalMisc, CastNodeAndColumnRefs) {
    EXPECT_EQ(Eval(*BoundExpr::Cast(I(7), LogicalType::Double())).GetDouble(), 7.0);
    EXPECT_EQ(Eval(*BoundExpr::Cast(S("12"), LogicalType::Integer())).GetInteger(), 12);
    EXPECT_TRUE(Throws(*BoundExpr::Cast(S("zz"), LogicalType::Integer()), ErrorCode::Type));
    const std::vector<Value> row = {Value::Integer(10), Value::Varchar("x"),
                                    Value::Null(LogicalType::Integer())};
    E sum = Bin(OperatorKind::Add, BoundExpr::ColumnRef(0, LogicalType::Integer()), I(5),
                LogicalType::Integer());
    EXPECT_EQ(EvaluateScalar(*sum, row).GetInteger(), 15);
    EXPECT_TRUE(
        EvaluateScalar(*Bin(OperatorKind::Add, BoundExpr::ColumnRef(2, LogicalType::Integer()),
                            I(1), LogicalType::Integer()),
                       row)
            .IsNull());
}

TEST(ScalarEvalMiscDeathTest, ContractViolations) {
    EXPECT_DEATH(EvaluateScalar(*BoundExpr::ColumnRef(3, LogicalType::Integer()), {}), "CDB_CHECK");
    try {
        Eval(*BoundExpr::Aggregate(AggregateKind::CountStar, {}, false, LogicalType::BigInt()));
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Internal);
    }
}

} // namespace cdb
