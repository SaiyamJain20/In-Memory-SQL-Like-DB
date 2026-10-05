// Differential tests for the vectorized ExpressionExecutor. The oracle is EvaluateScalar, the
// row-at-a-time reference interpreter (ADR 0004), which in turn is checked against DuckDB by the
// golden-expression test. Random, well-typed SQL expressions are generated as text, bound with
// the real binder (so implicit casts and desugaring are exercised), and evaluated over random
// chunks whose columns are flat, constant or dictionary vectors full of NULLs.
//
// Contract checked for every (expression, chunk):
//   * if the interpreter succeeds on every row, Execute succeeds and returns identical values;
//   * if the interpreter fails on some row, Execute fails too (the kernels are lazy exactly
//     where the interpreter is, so errors in unreachable branches never fire);
//   * for BOOLEAN expressions, Select returns exactly the rows that are TRUE.

#include "execution/expression_executor.h"

#include "main/connection.h"
#include "planner/logical_plan.h"
#include "planner/scalar_eval.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

// ------------------------------------------------------------------------------ expression text

class ExprGen {
  public:
    explicit ExprGen(Rng& rng) : rng_(rng) {}

    std::string Num(int depth) {
        if (depth <= 0 || Chance(rng_, 0.2)) {
            return Chance(rng_, 0.6) ? Pick({"i", "l", "d"}) : NumLiteral();
        }
        switch (RandBelow(rng_, 16)) {
        case 0:
        case 1:
        case 2:
            return "(" + Num(depth - 1) + " " + Pick({"+", "-", "*"}) + " " + Num(depth - 1) + ")";
        case 3:
            return "(" + Num(depth - 1) + " / " + Num(depth - 1) + ")";
        case 4:
            return "(" + Num(depth - 1) + " % " + Num(depth - 1) + ")";
        case 5:
            return "(-" + Num(depth - 1) + ")";
        case 6:
            return "abs(" + Num(depth - 1) + ")";
        case 7:
            return Pick({"floor", "ceil"}) + "(" + Num(depth - 1) + ")";
        case 8:
            return "round(" + Num(depth - 1) +
                   (Chance(rng_, 0.5)
                        ? ", " + std::to_string(static_cast<int>(RandBelow(rng_, 5)) - 1)
                        : "") +
                   ")";
        case 9:
            return "length(" + Str(depth - 1) + ")";
        case 10:
            return Pick({"year", "month", "day", "dayofweek", "dayofyear", "quarter"}) + "(" +
                   Date(depth - 1) + ")";
        case 11:
            return Case(depth, [&] { return Num(depth - 1); });
        case 12:
            return Coalesce(depth, [&] { return Num(depth - 1); });
        case 13:
            return "nullif(" + Num(depth - 1) + ", " + Num(depth - 1) + ")";
        case 14: {
            const std::string t = Pick({"INTEGER", "BIGINT", "DOUBLE"});
            switch (RandBelow(rng_, 5)) {
            case 0:
                return "CAST(" + Bool(depth - 1) + " AS " + t + ")";
            case 1:
                return "CAST(" + Str(depth - 1) + " AS " + t + ")";
            default:
                return "CAST(" + Num(depth - 1) + " AS " + t + ")";
            }
        }
        default:
            return "(" + Date(depth - 1) + " - " + Date(depth - 1) + ")";
        }
    }

    std::string Str(int depth) {
        if (depth <= 0 || Chance(rng_, 0.2)) {
            return Chance(rng_, 0.6) ? "s" : StrLiteral();
        }
        switch (RandBelow(rng_, 10)) {
        case 0:
        case 1:
            return "(" + Str(depth - 1) + " || " + Str(depth - 1) + ")";
        case 2:
        case 3: {
            std::string out = "substring(" + Str(depth - 1) + ", " + SmallInt();
            if (Chance(rng_, 0.6)) {
                out += ", " + SmallInt();
            }
            return out + ")";
        }
        case 4:
            return Pick({"upper", "lower"}) + "(" + Str(depth - 1) + ")";
        case 5:
            return Case(depth, [&] { return Str(depth - 1); });
        case 6:
            return Coalesce(depth, [&] { return Str(depth - 1); });
        case 7:
            return "nullif(" + Str(depth - 1) + ", " + Str(depth - 1) + ")";
        case 8:
            return "CAST(" + Num(depth - 1) + " AS VARCHAR)";
        default:
            return "CAST(" + (Chance(rng_, 0.5) ? Date(depth - 1) : Bool(depth - 1)) +
                   " AS VARCHAR)";
        }
    }

    std::string Date(int depth) {
        if (depth <= 0 || Chance(rng_, 0.25)) {
            return Chance(rng_, 0.6) ? "dt" : DateLiteral();
        }
        switch (RandBelow(rng_, 7)) {
        case 0:
        case 1:
            return "(" + Date(depth - 1) + " " + Pick({"+", "-"}) + " " + SmallInt() + ")";
        case 2:
            return "(" + Date(depth - 1) + " + INTERVAL '" + std::to_string(RandBelow(rng_, 40)) +
                   "' " + Pick({"DAY", "MONTH", "YEAR"}) + ")";
        case 3:
            return Case(depth, [&] { return Date(depth - 1); });
        case 4:
            return Coalesce(depth, [&] { return Date(depth - 1); });
        case 5:
            return "nullif(" + Date(depth - 1) + ", " + Date(depth - 1) + ")";
        default:
            return "CAST(" + (Chance(rng_, 0.5) ? "s" : StrLiteral()) + " AS DATE)";
        }
    }

    std::string Bool(int depth) {
        if (depth <= 0 || Chance(rng_, 0.15)) {
            return Chance(rng_, 0.6) ? "b" : Pick({"TRUE", "FALSE", "CAST(NULL AS BOOLEAN)"});
        }
        switch (RandBelow(rng_, 16)) {
        case 0:
        case 1:
        case 2:
        case 3:
            return Comparison(depth);
        case 4:
        case 5:
            return "(" + Bool(depth - 1) + " AND " + Bool(depth - 1) + ")";
        case 6:
        case 7:
            return "(" + Bool(depth - 1) + " OR " + Bool(depth - 1) + ")";
        case 8:
            return "(NOT " + Bool(depth - 1) + ")";
        case 9:
            return "(" + AnyOperand(depth - 1) +
                   (Chance(rng_, 0.5) ? " IS NULL)" : " IS NOT NULL)");
        case 10:
            return InList(depth);
        case 11:
            return "(" + Str(depth - 1) + (Chance(rng_, 0.3) ? " NOT LIKE " : " LIKE ") +
                   (Chance(rng_, 0.85) ? LikePattern() : Str(depth - 1)) + ")";
        case 12:
            return "(" + Num(depth - 1) + (Chance(rng_, 0.3) ? " NOT BETWEEN " : " BETWEEN ") +
                   Num(depth - 1) + " AND " + Num(depth - 1) + ")";
        case 13:
            return Case(depth, [&] { return Bool(depth - 1); });
        case 14:
            return Coalesce(depth, [&] { return Bool(depth - 1); });
        default:
            return "CAST(" + Num(depth - 1) + " AS BOOLEAN)";
        }
    }

    std::string Any(int depth) {
        switch (RandBelow(rng_, 4)) {
        case 0:
            return Num(depth);
        case 1:
            return Str(depth);
        case 2:
            return Date(depth);
        default:
            return Bool(depth);
        }
    }

  private:
    std::string Pick(std::initializer_list<const char*> options) {
        return *(options.begin() + RandBelow(rng_, options.size()));
    }

    std::string NumLiteral() {
        switch (RandBelow(rng_, 8)) {
        case 0:
            return "2147483647";
        case 1:
            return "9223372036854775807";
        case 2:
            return Pick({"0.5", "(-1.5)", "2.25", "1e10", "0.0"});
        case 3:
            return "CAST(NULL AS " + Pick({"INTEGER", "BIGINT", "DOUBLE"}) + ")";
        default:
            return std::to_string(static_cast<int>(RandBelow(rng_, 25)) - 4);
        }
    }

    std::string SmallInt() {
        return Chance(rng_, 0.85)
                   ? "(" + std::to_string(static_cast<int>(RandBelow(rng_, 14)) - 4) + ")"
                   : "(i % 7)";
    }

    std::string StrLiteral() {
        return Pick({"''", "'a'", "'ab'", "'abc'", "'Hello World'", "'\xC3\x84\xC3\x96z'", "'%'",
                     "'a b'", "'abcdefghijklmnopqrstuvwxyz'", "'2000-02-29'", "'42'", "'-7'",
                     "'1.5'", "'true'", "CAST(NULL AS VARCHAR)"});
    }

    std::string DateLiteral() {
        return Pick({"DATE '1970-01-01'", "DATE '2000-02-29'", "DATE '1999-12-31'",
                     "DATE '0001-01-01'", "DATE '9999-12-31'", "CAST(NULL AS DATE)"});
    }

    std::string LikePattern() {
        return Pick({"'a%'", "'%a'", "'%a%'", "'a_c'", "'%'", "''", "'abc'", "'%%'", "'_%'",
                     "'a%c'", "'%a%b%'", "'Hello%'", "'%World'", "'%o W%'", "'__'", "'_'",
                     "'%\xC3\x84%'"});
    }

    std::string Comparison(int depth) {
        const std::string op = Pick({"=", "<>", "<", "<=", ">", ">="});
        switch (RandBelow(rng_, 5)) {
        case 0:
            return "(" + Str(depth - 1) + " " + op + " " + Str(depth - 1) + ")";
        case 1:
            return "(" + Date(depth - 1) + " " + op + " " + Date(depth - 1) + ")";
        case 2:
            return "(" + Bool(depth - 1) + " " + op + " " + Bool(depth - 1) + ")";
        default:
            return "(" + Num(depth - 1) + " " + op + " " + Num(depth - 1) + ")";
        }
    }

    std::string AnyOperand(int depth) { return Any(depth); }

    std::string InList(int depth) {
        const bool negated = Chance(rng_, 0.3);
        std::string out = "(";
        std::vector<std::string> items;
        const size_t n = 1 + RandBelow(rng_, 5);
        switch (RandBelow(rng_, 3)) {
        case 0:
            out += Str(depth - 1);
            for (size_t k = 0; k < n; k++) {
                items.push_back(StrLiteral());
            }
            break;
        case 1:
            out += Date(depth - 1);
            for (size_t k = 0; k < n; k++) {
                items.push_back(DateLiteral());
            }
            break;
        default:
            out += Num(depth - 1);
            for (size_t k = 0; k < n; k++) {
                items.push_back(Chance(rng_, 0.8) ? NumLiteral() : Num(depth - 1));
            }
            break;
        }
        out += negated ? " NOT IN (" : " IN (";
        for (size_t k = 0; k < items.size(); k++) {
            out += (k ? ", " : "") + items[k];
        }
        return out + "))";
    }

    template <class F> std::string Case(int depth, F branch) {
        std::string out = "CASE";
        const bool simple = Chance(rng_, 0.3);
        if (simple) {
            out += " " + Num(depth - 1);
        }
        const size_t n = 1 + RandBelow(rng_, 3);
        for (size_t k = 0; k < n; k++) {
            out += " WHEN " + (simple ? Num(depth - 1) : Bool(depth - 1)) + " THEN " + branch();
        }
        if (Chance(rng_, 0.8)) {
            out += " ELSE " + branch();
        }
        return out + " END";
    }

    template <class F> std::string Coalesce(int, F arg) {
        std::string out = "coalesce(" + arg();
        const size_t n = 1 + RandBelow(rng_, 2);
        for (size_t k = 0; k < n; k++) {
            out += ", " + arg();
        }
        return out + ")";
    }

    Rng& rng_;
};

// ------------------------------------------------------------------------------ test data

const std::vector<LogicalType> kColumnTypes = {LogicalType::Boolean(), LogicalType::Integer(),
                                               LogicalType::BigInt(),  LogicalType::Double(),
                                               LogicalType::Date(),    LogicalType::Varchar()};

// Values that keep arithmetic mostly in range, so expressions get past overflow errors and the
// success path is exercised thoroughly; "extreme" mode uses the full-range generators instead.
Value BenignValue(Rng& rng, LogicalType type) {
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(RandBelow(rng, 2) == 1);
    case TypeId::Integer:
        return Value::Integer(static_cast<int32_t>(RandBelow(rng, 40)) - 8);
    case TypeId::BigInt:
        return Value::BigInt(Chance(rng, 0.9)
                                 ? static_cast<int64_t>(RandBelow(rng, 40)) - 8
                                 : static_cast<int64_t>(RandBelow(rng, 2000000)) * 1000003);
    case TypeId::Double: {
        static const double kPool[] = {0.0,  -0.0, 0.5, -1.5, 2.0,   3.25,    1e10,
                                       -7.0, 41.5, 1.0, NAN,  1e300, INFINITY};
        return Value::Double(kPool[RandBelow(rng, std::size(kPool))]);
    }
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(RandBelow(rng, 1600)) + 10000});
    case TypeId::Varchar: {
        static const char* const kPool[] = {
            "",   "a",  "ab",  "abc",  "Hello World", "\xC3\x84\xC3\x96z",          "%",   "a b",
            "42", "-7", "1.5", "true", "2000-02-29",  "abcdefghijklmnopqrstuvwxyz", "AbC", "ba"};
        if (Chance(rng, 0.3)) {
            std::string s;
            const size_t len = RandBelow(rng, 7);
            for (size_t k = 0; k < len; k++) {
                s += "abAB %_"[RandBelow(rng, 7)];
            }
            return Value::Varchar(s);
        }
        return Value::Varchar(kPool[RandBelow(rng, std::size(kPool))]);
    }
    }
    return Value::Null(type);
}

Value RandomCell(Rng& rng, LogicalType type, bool extreme, double null_probability) {
    if (Chance(rng, null_probability)) {
        return Value::Null(type);
    }
    return extreme ? test::RandomValue(rng, type, 0.0) : BenignValue(rng, type);
}

// Fills `chunk` with `count` random rows. Each column independently picks a vector format.
void FillChunk(Rng& rng, DataChunk& chunk, idx_t count, bool extreme) {
    chunk.Initialize(kColumnTypes);
    for (idx_t c = 0; c < kColumnTypes.size(); c++) {
        const LogicalType type = kColumnTypes[c];
        const double nulls = Chance(rng, 0.15) ? 0.0 : (Chance(rng, 0.1) ? 0.9 : 0.25);
        switch (RandBelow(rng, 4)) {
        case 0: { // constant
            Vector v = Vector::MakeConstant(RandomCell(rng, type, extreme, 0.2));
            chunk.column(c).Reference(v);
            break;
        }
        case 1: { // dictionary over a smaller child
            const idx_t child_rows = 1 + RandBelow(rng, std::max<idx_t>(count, 1));
            Vector child(type);
            for (idx_t r = 0; r < child_rows; r++) {
                child.SetValue(r, RandomCell(rng, type, extreme, nulls));
            }
            SelectionVector sel(std::max<idx_t>(count, 1));
            for (idx_t r = 0; r < count; r++) {
                sel.Set(r, static_cast<sel_t>(RandBelow(rng, child_rows)));
            }
            child.Slice(sel, count);
            chunk.column(c).Reference(child);
            break;
        }
        default: // flat
            for (idx_t r = 0; r < count; r++) {
                chunk.column(c).SetValue(r, RandomCell(rng, type, extreme, nulls));
            }
            break;
        }
    }
    chunk.SetCardinality(count);
}

idx_t RandomRowCount(Rng& rng) {
    switch (RandBelow(rng, 10)) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 2 + RandBelow(rng, 5);
    case 3:
        return kVectorSize;
    case 4:
        return kVectorSize - 1 - RandBelow(rng, 3);
    default:
        return 8 + RandBelow(rng, 120);
    }
}

// Equality that treats NaN == NaN and compares doubles by bits otherwise (so -0.0 vs 0.0 and
// every ulp of difference is caught).
bool Same(const Value& a, const Value& b) {
    if (a.type() != b.type() || a.IsNull() != b.IsNull()) {
        return false;
    }
    if (a.IsNull()) {
        return true;
    }
    if (a.type().id() == TypeId::Double) {
        const double x = a.GetDouble(), y = b.GetDouble();
        if (std::isnan(x) || std::isnan(y)) {
            return std::isnan(x) && std::isnan(y);
        }
        return std::memcmp(&x, &y, sizeof x) == 0;
    }
    return a == b;
}

// ------------------------------------------------------------------------------ the harness

struct Fixture {
    Database db;
    Connection conn{db};
    Fixture() {
        const QueryResult r = conn.Query(
            "CREATE TABLE t (b BOOLEAN, i INTEGER, l BIGINT, d DOUBLE, dt DATE, s VARCHAR)");
        EXPECT_TRUE(r.ok()) << r.error_message();
    }

    // Binds `SELECT <expr> FROM t` and returns the projected expression (ordinals refer to the
    // six table columns in order). Null if the generated text does not bind (type errors,
    // bind-time constant errors); those are skipped.
    LogicalPtr Bind(const std::string& expr) {
        try {
            return conn.Plan("SELECT " + expr + " FROM t");
        } catch (const Error&) {
            return nullptr;
        }
    }
};

const BoundExpr& Projected(const LogicalOperator& plan) {
    EXPECT_EQ(plan.kind, LogicalKind::Projection);
    const auto& p = static_cast<const LogicalProjection&>(plan);
    EXPECT_EQ(p.exprs.size(), 1U);
    return *p.exprs[0];
}

struct Reference {
    std::vector<Value> values; // per row; only meaningful when !failed
    bool failed = false;
    std::string error;
};

Reference RunInterpreter(const BoundExpr& expr, const DataChunk& chunk) {
    Reference ref;
    std::vector<Value> row(chunk.ColumnCount(), Value::Null(LogicalType::Integer()));
    for (idx_t r = 0; r < chunk.size(); r++) {
        for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
            row[c] = chunk.GetValue(c, r);
        }
        try {
            ref.values.push_back(EvaluateScalar(expr, row));
        } catch (const Error& e) {
            ref.failed = true;
            ref.error = e.what();
            return ref;
        }
    }
    return ref;
}

struct Outcome {
    uint64_t compared = 0, errors = 0, bound = 0, skipped = 0;
};

// The executor evaluates a predicate in "select position" (the top of Select, a CASE condition)
// by narrowing: in `a AND b`, rows where `a` is FALSE *or NULL* never reach `b`, because they can
// never be TRUE (DuckDB does the same). The interpreter still evaluates `b` for NULL `a`, so a
// run-time error in `b` on such a row is raised by the interpreter but not by the executor. That
// is the only place the two may legitimately differ in error behaviour; this finds expressions
// where it can happen so the test tolerates "interpreter failed, executor succeeded" for them
// (and only them).
bool MayNarrow(const BoundExpr& e, bool select_position) {
    if (e.kind == BoundKind::Operator && (e.op == OperatorKind::And || e.op == OperatorKind::Or) &&
        select_position) {
        return true;
    }
    for (size_t k = 0; k < e.children.size(); k++) {
        bool child_in_select = false;
        if (e.kind == BoundKind::Case) {
            child_in_select = k + 1 < e.children.size() && k % 2 == 0; // the WHEN conditions
        } else if (e.kind == BoundKind::Operator && e.op == OperatorKind::Not) {
            child_in_select = select_position;
        } else if (e.kind == BoundKind::Operator &&
                   (e.op == OperatorKind::And || e.op == OperatorKind::Or)) {
            child_in_select = false; // value position: both sides evaluated like the interpreter
        }
        if (MayNarrow(*e.children[k], child_in_select)) {
            return true;
        }
    }
    return false;
}

// For a failure report: runs the executor on row `r` alone, to tell a per-row kernel bug (the
// single-row result is wrong too) from a batching bug (it is right).
std::string IsolatedRow(const BoundExpr& expr, const DataChunk& chunk, idx_t r) {
    DataChunk one;
    one.Initialize(chunk.types());
    for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
        one.SetValue(c, 0, chunk.GetValue(c, r));
    }
    one.SetCardinality(1);
    std::string cells;
    for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
        cells += (c ? " | " : "") + chunk.GetValue(c, r).ToString();
        const Value v = chunk.GetValue(c, r);
        if (!v.IsNull() && v.type().id() == TypeId::Varchar) {
            static const char* hex = "0123456789abcdef";
            cells += " [hex ";
            for (unsigned char ch : v.GetVarchar()) {
                cells += hex[ch >> 4];
                cells += hex[ch & 15];
            }
            cells += "]";
        }
    }
    cells = "  row cells: " + cells + "\n";
    try {
        ExpressionExecutor ex(expr);
        Vector out(expr.type);
        ex.Execute(one, out);
        return cells + "  row alone through the executor: " + out.GetValue(0).ToString();
    } catch (const Error& e) {
        return cells + "  row alone through the executor failed: " + e.what();
    }
}

// Checks one expression against one chunk; returns false (after reporting) on a mismatch.
bool CheckOne(const std::string& sql, const BoundExpr& expr, const DataChunk& chunk,
              Outcome& stats) {
    const Reference ref = RunInterpreter(expr, chunk);
    ExpressionExecutor ex(expr);

    Vector out(expr.type);
    bool threw = false;
    std::string thrown;
    try {
        ex.Execute(chunk, out);
    } catch (const Error& e) {
        threw = true;
        thrown = e.what();
    }
    const bool tolerated = ref.failed && !threw && MayNarrow(expr, false);
    if (ref.failed != threw && !tolerated) {
        ADD_FAILURE() << sql << "\n  interpreter "
                      << (ref.failed ? "failed: " + ref.error : "succeeded") << "\n  executor    "
                      << (threw ? "failed: " + thrown : "succeeded") << "\n"
                      << chunk.ToString().substr(0, 1500);
        return false;
    }
    if (ref.failed) {
        stats.errors++;
    } else {
        out.Verify(chunk.size());
        for (idx_t r = 0; r < chunk.size(); r++) {
            const Value got = out.GetValue(r);
            if (!Same(got, ref.values[r])) {
                ADD_FAILURE() << sql << "\n  row " << r << ": executor " << got.ToString() << " ("
                              << got.type().ToString() << ") vs interpreter "
                              << ref.values[r].ToString() << " (" << ref.values[r].type().ToString()
                              << ")\n"
                              << IsolatedRow(expr, chunk, r) << "\n"
                              << chunk.ToString().substr(0, 1500);
                return false;
            }
        }
        stats.compared += chunk.size();
    }

    if (expr.type.id() != TypeId::Boolean || ref.failed) {
        return true; // (Select may skip rows the interpreter evaluates; either outcome is legal)
    }
    ExpressionExecutor sel_ex(expr);
    SelectionVector sel;
    idx_t n = 0;
    try {
        n = sel_ex.Select(chunk, sel);
    } catch (const Error& e) {
        ADD_FAILURE() << sql << "\n  Select failed but the interpreter succeeded: " << e.what()
                      << "\n"
                      << chunk.ToString().substr(0, 1500);
        return false;
    }
    std::vector<idx_t> expected;
    for (idx_t r = 0; r < chunk.size(); r++) {
        if (!ref.values[r].IsNull() && ref.values[r].GetBoolean()) {
            expected.push_back(r);
        }
    }
    bool ok = n == expected.size();
    for (idx_t j = 0; ok && j < n; j++) {
        ok = sel[j] == expected[j];
    }
    if (!ok) {
        ADD_FAILURE() << sql << "\n  Select returned " << n << " rows, expected " << expected.size()
                      << "\n"
                      << chunk.ToString().substr(0, 1500);
        return false;
    }
    return true;
}

uint64_t Iterations() {
    const char* env = std::getenv("CDB_EXEC_FUZZ_ITERS");
    return env ? std::strtoull(env, nullptr, 10) : 1200;
}

} // namespace

// ------------------------------------------------------------------------------ randomized

class ExecutorDifferential : public ::testing::TestWithParam<uint64_t> {};

TEST_P(ExecutorDifferential, RandomExpressionsMatchInterpreter) {
    Rng rng(GetParam());
    Fixture fx;
    ExprGen gen(rng);
    Outcome stats;
    const uint64_t iterations = Iterations();
    for (uint64_t it = 0; it < iterations; it++) {
        const int depth = 1 + static_cast<int>(RandBelow(rng, 4));
        const std::string sql = gen.Any(depth);
        const LogicalPtr plan = fx.Bind(sql);
        if (!plan) {
            stats.skipped++;
            continue;
        }
        stats.bound++;
        const BoundExpr& expr = Projected(*plan);
        for (int k = 0; k < 3; k++) {
            DataChunk chunk;
            FillChunk(rng, chunk, RandomRowCount(rng), /*extreme=*/Chance(rng, 0.25));
            if (!CheckOne(sql, expr, chunk, stats)) {
                return; // one precise failure is more useful than a flood
            }
        }
    }
    // The generator must produce mostly bindable expressions, and most of them must evaluate
    // without error, or the comparison is not testing much.
    EXPECT_GT(stats.bound, iterations * 6 / 10) << "too many generated expressions failed to bind";
    EXPECT_GT(stats.compared, 0U);
    EXPECT_LT(stats.errors, stats.bound * 3 * 6 / 10) << "too many evaluations ended in errors";
}

// ------------------------------------------------------------------------------ directed

namespace {

// Rows are {b, i, l, d, dt, s}; unspecified columns are NULL.
struct Row {
    Value b = Value::Null(LogicalType::Boolean());
    Value i = Value::Null(LogicalType::Integer());
    Value l = Value::Null(LogicalType::BigInt());
    Value d = Value::Null(LogicalType::Double());
    Value dt = Value::Null(LogicalType::Date());
    Value s = Value::Null(LogicalType::Varchar());
};

Row IntRow(int32_t i) {
    Row r;
    r.i = Value::Integer(i);
    return r;
}

DataChunk MakeChunk(const std::vector<Row>& rows) {
    DataChunk chunk;
    chunk.Initialize(kColumnTypes);
    for (idx_t r = 0; r < rows.size(); r++) {
        const Value* cells[] = {&rows[r].b, &rows[r].i,  &rows[r].l,
                                &rows[r].d, &rows[r].dt, &rows[r].s};
        for (idx_t c = 0; c < 6; c++) {
            chunk.column(c).SetValue(r, *cells[c]);
        }
    }
    chunk.SetCardinality(rows.size());
    return chunk;
}

struct Directed {
    Fixture fx;
    LogicalPtr plan;

    const BoundExpr& Bind(const std::string& sql) {
        plan = fx.Bind(sql);
        EXPECT_NE(plan, nullptr) << sql;
        return Projected(*plan);
    }
    std::vector<Value> Run(const std::string& sql, const DataChunk& chunk) {
        const BoundExpr& e = Bind(sql);
        ExpressionExecutor ex(e);
        Vector out(e.type);
        ex.Execute(chunk, out);
        std::vector<Value> v;
        for (idx_t r = 0; r < chunk.size(); r++) {
            v.push_back(out.GetValue(r));
        }
        return v;
    }
    std::vector<idx_t> Select(const std::string& sql, const DataChunk& chunk) {
        const BoundExpr& e = Bind(sql);
        ExpressionExecutor ex(e);
        SelectionVector sel;
        const idx_t n = ex.Select(chunk, sel);
        std::vector<idx_t> v;
        for (idx_t j = 0; j < n; j++) {
            v.push_back(sel[j]);
        }
        return v;
    }
};

} // namespace

TEST(ExecutorDirected, ArithmeticOverflowIsAnErrorOnlyWhereEvaluated) {
    Directed t;
    const DataChunk chunk = MakeChunk({IntRow(1), IntRow(2), IntRow(3)});
    EXPECT_THROW(t.Run("i * 2147483647", chunk), Error);
    EXPECT_NO_THROW(t.Run("i * 1000", chunk));
    EXPECT_THROW(t.Run("i + 2147483647", MakeChunk({IntRow(1)})), Error);
    EXPECT_NO_THROW(t.Run("i + 2147483646", MakeChunk({IntRow(1)})));
    // NULL operands never overflow.
    EXPECT_NO_THROW(t.Run("i * 2147483647", MakeChunk({Row{}, Row{}})));
}

TEST(ExecutorDirected, CaseIsLazy) {
    Directed t;
    const std::string expr = "CASE WHEN i < 100 THEN i ELSE i * 2147483647 END";
    const auto ok = t.Run(expr, MakeChunk({IntRow(1), IntRow(50), Row{}}));
    EXPECT_EQ(ok[0], Value::Integer(1));
    EXPECT_EQ(ok[1], Value::Integer(50));
    EXPECT_TRUE(ok[2].IsNull()); // i IS NULL: condition is NULL, falls to ELSE, NULL * x = NULL
    EXPECT_THROW(t.Run(expr, MakeChunk({IntRow(1), IntRow(1000)})), Error);
}

TEST(ExecutorDirected, CaseTakesTheFirstMatchingBranch) {
    Directed t;
    const auto v =
        t.Run("CASE WHEN i < 0 THEN 'neg' WHEN i < 10 THEN 'small' WHEN i < 100 THEN 'mid' "
              "ELSE 'big' END",
              MakeChunk({IntRow(-5), IntRow(5), IntRow(50), IntRow(500), Row{}}));
    EXPECT_EQ(v[0], Value::Varchar("neg"));
    EXPECT_EQ(v[1], Value::Varchar("small"));
    EXPECT_EQ(v[2], Value::Varchar("mid"));
    EXPECT_EQ(v[3], Value::Varchar("big"));
    EXPECT_EQ(v[4], Value::Varchar("big")); // NULL condition does not match
}

TEST(ExecutorDirected, AndOrAreLazy) {
    Directed t;
    const DataChunk small = MakeChunk({IntRow(1), IntRow(2)});
    EXPECT_NO_THROW(t.Run("i >= 100 AND i * 2147483647 > 0", small));
    EXPECT_NO_THROW(t.Run("i < 100 OR i * 2147483647 > 0", small));
    EXPECT_THROW(t.Run("i < 100 AND i * 2147483647 > 0", small), Error);
    EXPECT_THROW(t.Run("i >= 100 OR i * 2147483647 > 0", small), Error);
    // A NULL left side must still evaluate the right side (it could decide the result).
    Row null_b;
    null_b.i = Value::Integer(2);
    EXPECT_THROW(t.Run("b AND i * 2147483647 > 0", MakeChunk({null_b})), Error);
}

TEST(ExecutorDirected, ThreeValuedLogic) {
    Directed t;
    std::vector<Row> rows;
    for (const bool has_b : {false, true}) {
        for (const int bv : {0, 1}) {
            Row r;
            if (has_b) {
                r.b = Value::Boolean(bv == 1);
            }
            rows.push_back(r);
        }
    }
    // rows: NULL, NULL, FALSE, TRUE (the first two are both NULL)
    const DataChunk chunk = MakeChunk(rows);
    const auto and_v = t.Run("b AND CAST(NULL AS BOOLEAN)", chunk);
    EXPECT_TRUE(and_v[0].IsNull());
    EXPECT_FALSE(and_v[2].IsNull());
    EXPECT_FALSE(and_v[2].GetBoolean()); // FALSE AND NULL = FALSE
    EXPECT_TRUE(and_v[3].IsNull());      // TRUE AND NULL = NULL
    const auto or_v = t.Run("b OR CAST(NULL AS BOOLEAN)", chunk);
    EXPECT_TRUE(or_v[0].IsNull());
    EXPECT_TRUE(or_v[2].IsNull());     // FALSE OR NULL = NULL
    EXPECT_TRUE(or_v[3].GetBoolean()); // TRUE OR NULL = TRUE
    const auto not_v = t.Run("NOT b", chunk);
    EXPECT_TRUE(not_v[0].IsNull());
    EXPECT_TRUE(not_v[2].GetBoolean());
    EXPECT_FALSE(not_v[3].GetBoolean());
}

TEST(ExecutorDirected, CoalesceIsLazyAndPicksTheFirstNonNull) {
    Directed t;
    const auto v =
        t.Run("coalesce(i, i * 2147483647, 7)", MakeChunk({IntRow(3), Row{}, IntRow(9)}));
    EXPECT_EQ(v[0], Value::Integer(3));
    EXPECT_EQ(v[1], Value::Integer(7)); // NULL * x = NULL, so the 7 is reached
    EXPECT_EQ(v[2], Value::Integer(9));
    // All arguments NULL -> NULL.
    const auto n = t.Run("coalesce(i, CAST(NULL AS INTEGER))", MakeChunk({Row{}}));
    EXPECT_TRUE(n[0].IsNull());
}

TEST(ExecutorDirected, EmptyChunkEvaluatesToNothingForEveryShape) {
    Directed t;
    const DataChunk empty = MakeChunk({});
    for (const char* sql : {"i", "i + 1", "1 + 1", "b AND TRUE", "s || 'x'",
                            "CASE WHEN b THEN i ELSE 2 END", "coalesce(i, 1)", "substring(s, 1, 2)",
                            "s LIKE '%a'", "CAST(i AS VARCHAR)", "i IN (1, 2)", "dt + 1"}) {
        EXPECT_TRUE(t.Run(sql, empty).empty()) << sql;
    }
    EXPECT_TRUE(t.Select("b", empty).empty());
    EXPECT_TRUE(t.Select("TRUE", empty).empty());
}

TEST(ExecutorDirected, ColumnReferenceAndConstantResultsAreZeroCopy) {
    Directed t;
    DataChunk chunk;
    chunk.Initialize(kColumnTypes);
    Vector child(LogicalType::Integer());
    for (idx_t r = 0; r < 4; r++) {
        child.SetValue(r, Value::Integer(static_cast<int32_t>(r) * 10));
    }
    SelectionVector sel(3);
    sel.Set(0, 3);
    sel.Set(1, 0);
    sel.Set(2, 3);
    child.Slice(sel, 3);
    chunk.column(1).Reference(child);
    chunk.SetCardinality(3);

    const BoundExpr& e = t.Bind("i");
    ExpressionExecutor ex(e);
    Vector out(e.type);
    ex.Execute(chunk, out);
    EXPECT_EQ(out.format(), VectorFormat::Dictionary) << "a column reference must not flatten";
    EXPECT_EQ(out.GetValue(0), Value::Integer(30));
    EXPECT_EQ(out.GetValue(1), Value::Integer(0));
    EXPECT_EQ(out.GetValue(2), Value::Integer(30));

    const BoundExpr& k = t.Bind("i + 1");
    ExpressionExecutor ex2(k);
    Vector out2(k.type);
    ex2.Execute(chunk, out2);
    EXPECT_EQ(out2.GetValue(0).ToString(), "31");
    EXPECT_EQ(out2.GetValue(1).ToString(), "1");
    EXPECT_EQ(out2.GetValue(2).ToString(), "31");
}

TEST(ExecutorDirected, ConstantExpressionYieldsAConstantVector) {
    Directed t;
    const BoundExpr& e = t.Bind("1 + 1");
    ASSERT_EQ(e.kind, BoundKind::Constant) << "the binder folds constants";
    ExpressionExecutor ex(e);
    Vector out(e.type);
    const DataChunk chunk = MakeChunk({IntRow(1), IntRow(2), IntRow(3)});
    ex.Execute(chunk, out);
    EXPECT_EQ(out.format(), VectorFormat::Constant);
    for (idx_t r = 0; r < 3; r++) {
        EXPECT_EQ(out.GetValue(r).ToString(), "2");
    }
}

TEST(ExecutorDirected, SelectDropsNullAndFalseRowsInOrder) {
    Directed t;
    std::vector<Row> rows;
    for (const int32_t v : {5, -1, 7, 0, 9, 2}) {
        rows.push_back(IntRow(v));
    }
    rows.insert(rows.begin() + 2, Row{}); // a NULL in the middle
    const DataChunk chunk = MakeChunk(rows);
    EXPECT_EQ(t.Select("i > 1", chunk), (std::vector<idx_t>{0, 3, 5, 6}));
    EXPECT_EQ(t.Select("NOT (i > 1)", chunk), (std::vector<idx_t>{1, 4}));
    EXPECT_EQ(t.Select("i IS NULL", chunk), (std::vector<idx_t>{2}));
    EXPECT_EQ(t.Select("i IS NOT NULL", chunk), (std::vector<idx_t>{0, 1, 3, 4, 5, 6}));
    EXPECT_EQ(t.Select("i > 1 AND i < 9", chunk), (std::vector<idx_t>{0, 3, 6}));
    EXPECT_EQ(t.Select("i > 6 OR i < 0", chunk), (std::vector<idx_t>{1, 3, 5}));
    EXPECT_EQ(t.Select("NOT (i > 6 OR i < 0)", chunk), (std::vector<idx_t>{0, 4, 6}));
    EXPECT_EQ(t.Select("i BETWEEN 0 AND 5", chunk), (std::vector<idx_t>{0, 4, 6}));
    EXPECT_EQ(t.Select("TRUE", chunk).size(), chunk.size());
    EXPECT_TRUE(t.Select("FALSE", chunk).empty());
    EXPECT_TRUE(t.Select("CAST(NULL AS BOOLEAN)", chunk).empty());
}

TEST(ExecutorDirected, SelectOverDictionaryAndConstantInputs) {
    Directed t;
    DataChunk chunk;
    chunk.Initialize(kColumnTypes);
    Vector child(LogicalType::Integer());
    for (idx_t r = 0; r < 4; r++) {
        child.SetValue(r, Value::Integer(static_cast<int32_t>(r)));
    }
    child.Validity().SetInvalid(2);
    SelectionVector sel(5);
    for (idx_t r = 0; r < 5; r++) {
        sel.Set(r, static_cast<sel_t>(3 - (r % 4))); // 3,2,1,0,3
    }
    child.Slice(sel, 5);
    chunk.column(1).Reference(child);
    Vector konst = Vector::MakeConstant(Value::Boolean(true));
    chunk.column(0).Reference(konst);
    chunk.SetCardinality(5);
    EXPECT_EQ(t.Select("i >= 1", chunk), (std::vector<idx_t>{0, 2, 4}));
    EXPECT_EQ(t.Select("b AND i >= 1", chunk), (std::vector<idx_t>{0, 2, 4}));
    EXPECT_EQ(t.Select("NOT b OR i = 0", chunk), (std::vector<idx_t>{3}));
}

TEST(ExecutorDirected, SelectAndRunAgreeAcrossManyRandomPredicates) {
    // Select must pick exactly the TRUE rows Execute reports, for predicates built only from the
    // shapes the narrowing logic special-cases (comparisons, NOT, AND, OR, IS NULL), on chunks
    // with NULLs and every vector format.
    Rng rng(99);
    Directed t;
    ExprGen gen(rng);
    for (int it = 0; it < 600; it++) {
        const std::string pred = gen.Bool(1 + static_cast<int>(RandBelow(rng, 3)));
        const LogicalPtr plan = t.fx.Bind(pred);
        if (!plan) {
            continue;
        }
        const BoundExpr& e = Projected(*plan);
        DataChunk chunk;
        FillChunk(rng, chunk, RandomRowCount(rng), false);
        ExpressionExecutor value_ex(e), select_ex(e);
        Vector out(e.type);
        std::vector<idx_t> expected;
        try {
            value_ex.Execute(chunk, out);
        } catch (const Error&) {
            continue;
        }
        for (idx_t r = 0; r < chunk.size(); r++) {
            const Value v = out.GetValue(r);
            if (!v.IsNull() && v.GetBoolean()) {
                expected.push_back(r);
            }
        }
        SelectionVector sel;
        idx_t n = 0;
        try {
            n = select_ex.Select(chunk, sel);
        } catch (const Error&) {
            continue; // narrowing can only skip errors, never add them; a throw here is a bug:
        }
        ASSERT_EQ(n, expected.size()) << pred;
        for (idx_t j = 0; j < n; j++) {
            ASSERT_EQ(sel[j], expected[j]) << pred;
        }
    }
}

TEST(ExecutorDirected, AggregatesCannotBeEvaluatedRowWise) {
    std::vector<BoundExprPtr> args;
    args.push_back(BoundExpr::ColumnRef(1, LogicalType::Integer()));
    const BoundExprPtr agg =
        BoundExpr::Aggregate(AggregateKind::Sum, std::move(args), false, LogicalType::BigInt());
    ExpressionExecutor ex(*agg);
    Vector out(LogicalType::BigInt());
    EXPECT_THROW(ex.Execute(MakeChunk({IntRow(1)}), out), Error);
}

TEST(ExecutorDirected, ExecutorIsReusableAcrossChunks) {
    // Scratch vectors persist between calls; stale rows from a bigger chunk must not leak into a
    // later, smaller one, and NULLs must not stick.
    Directed t;
    const BoundExpr& e = t.Bind("CASE WHEN i > 1 THEN i * 2 ELSE coalesce(i, -1) END");
    ExpressionExecutor ex(e);
    Vector out(e.type);
    ex.Execute(MakeChunk({IntRow(5), Row{}, IntRow(0), IntRow(9)}), out);
    EXPECT_EQ(out.GetValue(0), Value::Integer(10));
    EXPECT_EQ(out.GetValue(1), Value::Integer(-1));
    EXPECT_EQ(out.GetValue(2), Value::Integer(0));
    EXPECT_EQ(out.GetValue(3), Value::Integer(18));
    ex.Execute(MakeChunk({IntRow(3)}), out);
    EXPECT_EQ(out.GetValue(0), Value::Integer(6));
    ex.Execute(MakeChunk({Row{}, IntRow(4)}), out);
    EXPECT_EQ(out.GetValue(0), Value::Integer(-1));
    EXPECT_EQ(out.GetValue(1), Value::Integer(8));
}

INSTANTIATE_TEST_SUITE_P(Seeds, ExecutorDifferential, ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8));

} // namespace cdb
