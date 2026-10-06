// Tests for the logical optimizer (filter pushdown, join ordering, column pruning).
//
//  * Plan-shape tests lock in the optimized plans of representative queries (reviewed by hand).
//  * The equivalence test runs thousands of random queries - joins of every kind, derived tables,
//    aggregates, DISTINCT, ORDER BY/LIMIT, NULL-heavy data - with the optimizer ON and OFF and
//    requires identical results. With it off the plan executes exactly as the binder built it, so
//    a rewrite that changes meaning shows up as a difference.

#include "planner/optimizer.h"

#include "main/connection.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

struct Env {
    Database db;
    Connection conn{db};

    void Run(const std::string& sql) {
        const QueryResult r = conn.Query(sql);
        ASSERT_TRUE(r.ok()) << sql.substr(0, 200) << "\n" << r.error_message();
    }

    // Row counts matter for join ordering: big (1000) > mid (100) > small (5).
    void SizedTables() {
        std::string big, mid, small;
        for (int i = 0; i < 1000; i++) {
            big += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                   std::to_string(i % 50) + ",'" + (i % 3 ? "x" : "y") + "')";
        }
        for (int i = 0; i < 100; i++) {
            mid += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                   std::to_string(i * 2) + ")";
        }
        for (int i = 0; i < 5; i++) {
            small += (i ? "," : "") + std::string("(") + std::to_string(i) + ",'z" +
                     std::to_string(i) + "')";
        }
        Run("CREATE TABLE big (k INTEGER, v INTEGER, s VARCHAR)");
        Run("CREATE TABLE mid (k INTEGER, w INTEGER)");
        Run("CREATE TABLE small (k INTEGER, z VARCHAR)");
        Run("INSERT INTO big VALUES " + big);
        Run("INSERT INTO mid VALUES " + mid);
        Run("INSERT INTO small VALUES " + small);
    }

    std::string Explain(const std::string& sql) {
        const QueryResult r = conn.Query("EXPLAIN " + sql);
        EXPECT_TRUE(r.ok()) << r.error_message();
        std::string out;
        for (idx_t i = 0; i < r.RowCount(); i++) {
            out += r.GetValue(0, i).GetVarchar() + "\n";
        }
        return out;
    }
};

} // namespace

// ---------------------------------------------------------------------------------- plan shapes

TEST(OptimizerShapes, PredicatesMoveToTheScansOfAnInnerJoin) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(env.Explain(
                  "SELECT * FROM big, small WHERE big.k = small.k AND big.v > 5 AND small.z = 'x'"),
              "PROJECT [k, v, s, k, z]\n"
              "  JOIN INNER ON (big.k = small.k)\n"
              "    FILTER (v > 5)\n"
              "      SCAN big [k, v, s] prune(v > 5)\n"
              "    FILTER (z = 'x')\n"
              "      SCAN small [k, z] prune(z = x)\n");
}

TEST(OptimizerShapes, OuterJoinsOnlyReceivePredicatesThatKeepTheirMeaning) {
    Env env;
    env.SizedTables();
    // WHERE on the preserved side goes down; WHERE on the padded side must stay above the join.
    EXPECT_EQ(env.Explain("SELECT big.k FROM big LEFT JOIN small ON big.k = small.k "
                          "WHERE big.v > 5 AND small.z = 'x'"),
              "PROJECT [big.k AS k]\n"
              "  FILTER (z = 'x')\n"
              "    JOIN LEFT ON (big.k = small.k)\n"
              "      FILTER (v > 5)\n"
              "        SCAN big [k, v] prune(v > 5)\n"
              "      SCAN small [k, z]\n");
    // ON conjuncts: the padded side's go down, the preserved side's must stay in the condition.
    EXPECT_EQ(
        env.Explain("SELECT big.k FROM big LEFT JOIN small ON big.k = small.k AND small.z = 'z1' "
                    "AND big.v > 3"),
        "PROJECT [big.k AS k]\n"
        "  JOIN LEFT ON ((big.k = small.k) AND (v > 3))\n"
        "    SCAN big [k, v]\n"
        "    FILTER (z = 'z1')\n"
        "      SCAN small [k, z] prune(z = z1)\n");
}

TEST(OptimizerShapes, ZoneMapHintsOnlyForColumnVersusConstant) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(env.Explain("SELECT k FROM big WHERE v > 5"),
              "PROJECT [k]\n  FILTER (v > 5)\n    SCAN big [k, v] prune(v > 5)\n");
    EXPECT_EQ(env.Explain("SELECT k FROM big WHERE 5 < v"), // constant on the left: operator flips
              "PROJECT [k]\n  FILTER (5 < v)\n    SCAN big [k, v] prune(v > 5)\n");
    EXPECT_EQ(env.Explain("SELECT k FROM big WHERE v = 3 AND k <= 10"),
              "PROJECT [k]\n  FILTER ((v = 3) AND (k <= 10))\n    SCAN big [k, v] prune(v = 3) "
              "prune(k <= 10)\n");
    EXPECT_EQ(env.Explain("SELECT k FROM big WHERE v + 1 > 5"), // an expression, not a column
              "PROJECT [k]\n  FILTER ((v + 1) > 5)\n    SCAN big [k, v]\n");
    EXPECT_EQ(env.Explain("SELECT k FROM big WHERE v > k"), // two columns
              "PROJECT [k]\n  FILTER (v > k)\n    SCAN big [k, v]\n");
}

TEST(OptimizerShapes, FiltersMoveThroughProjectionsAndGroupKeys) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(env.Explain("SELECT * FROM (SELECT v + 1 AS w FROM big) q WHERE w > 10"),
              "PROJECT [w]\n"
              "  PROJECT [(v + 1) AS w]\n"
              "    FILTER ((v + 1) > 10)\n"
              "      SCAN big [v]\n");
    // HAVING on a group column is a WHERE in disguise; HAVING on an aggregate has to stay above.
    EXPECT_EQ(env.Explain("SELECT s, count(*) FROM big GROUP BY s HAVING s = 'x' AND count(*) > 1"),
              "PROJECT [s, count(*)]\n"
              "  FILTER (count(*) > 1::BIGINT)\n"
              "    AGGREGATE groups=[s] aggregates=[count(*)]\n"
              "      FILTER (s = 'x')\n"
              "        SCAN big [s] prune(s = x)\n");
    EXPECT_EQ(
        env.Explain("SELECT DISTINCT s FROM big WHERE v > 10"),
        "DISTINCT\n  PROJECT [s]\n    FILTER (v > 10)\n      SCAN big [v, s] prune(v > 10)\n");
}

TEST(OptimizerShapes, LimitSitsDirectlyOnOrderByAndColumnsArePruned) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(env.Explain("SELECT k FROM big ORDER BY v LIMIT 5"), "PROJECT [k]\n"
                                                                   "  LIMIT 5\n"
                                                                   "    ORDER BY v ASC NULLS LAST\n"
                                                                   "      PROJECT [k, v]\n"
                                                                   "        SCAN big [k, v]\n");
    EXPECT_EQ(
        env.Explain("SELECT count(*) FROM big"),
        "PROJECT [count(*)]\n  AGGREGATE groups=[] aggregates=[count(*)]\n    SCAN big [k]\n");
    EXPECT_EQ(env.Explain("SELECT v FROM big WHERE k > 1"),
              "PROJECT [v]\n  FILTER (k > 1)\n    SCAN big [k, v] prune(k > 1)\n");
}

TEST(OptimizerShapes, JoinOrderStartsFromTheLargestAndAvoidsCrossProducts) {
    Env env;
    env.SizedTables();
    // Written as small, big, mid: the plan probes with big, builds mid, then small - never a cross
    // join.
    EXPECT_EQ(
        env.Explain("SELECT small.z FROM small, big, mid WHERE small.k = mid.k AND mid.k = big.k"),
        "PROJECT [z]\n"
        "  PROJECT [z]\n"
        "    JOIN INNER ON (small.k = mid.k)\n"
        "      JOIN INNER ON (mid.k = big.k)\n"
        "        SCAN big [k]\n"
        "        SCAN mid [k]\n"
        "      SCAN small [k, z]\n");
    // Listed as big, mid, small the leaves already come out in join order, so there is no
    // column-restoring projection - but the join tree is the same.
    EXPECT_EQ(
        env.Explain("SELECT small.z FROM big, mid, small WHERE small.k = mid.k AND mid.k = big.k"),
        "PROJECT [z]\n"
        "  JOIN INNER ON (small.k = mid.k)\n"
        "    JOIN INNER ON (mid.k = big.k)\n"
        "      SCAN big [k]\n"
        "      SCAN mid [k]\n"
        "    SCAN small [k, z]\n");
}

TEST(OptimizerShapes, JoinOrderWeighsPredicateSelectivityNotJustRelationSize) {
    // The TPC-H Q5 trap. `cust` is the smaller relation, but its only link to `fact` before `ords`
    // joins is `cust.nat = fact.nat`, a 25-valued key: each fact row would match ~4 customers, so
    // the intermediate result quadruples. `ords` is bigger but joins on a unique key and keeps the
    // result at 2000 rows, so it must come first; cust then joins on both of its keys.
    Env env;
    env.Run("CREATE TABLE fact (id INTEGER, ord INTEGER, nat INTEGER)");
    env.Run("CREATE TABLE ords (ord INTEGER, cust INTEGER)");
    env.Run("CREATE TABLE cust (cust INTEGER, nat INTEGER)");
    std::string fact, ords, cust;
    for (int i = 0; i < 2000; i++) {
        fact += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                std::to_string(i % 500) + "," + std::to_string(i % 25) + ")";
    }
    for (int i = 0; i < 500; i++) {
        ords += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                std::to_string(i % 100) + ")";
    }
    for (int i = 0; i < 100; i++) {
        cust += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                std::to_string(i % 25) + ")";
    }
    env.Run("INSERT INTO fact VALUES " + fact);
    env.Run("INSERT INTO ords VALUES " + ords);
    env.Run("INSERT INTO cust VALUES " + cust);
    EXPECT_EQ(
        env.Explain("SELECT fact.id FROM fact, ords, cust "
                    "WHERE fact.ord = ords.ord AND ords.cust = cust.cust AND cust.nat = fact.nat"),
        "PROJECT [id]\n"
        "  JOIN INNER ON ((ords.cust = cust.cust) AND (cust.nat = fact.nat))\n"
        "    JOIN INNER ON (fact.ord = ords.ord)\n"
        "      SCAN fact [id, ord, nat]\n"
        "      SCAN ords [ord, cust]\n"
        "    SCAN cust [cust, nat]\n");
}

TEST(OptimizerShapes, OrBranchesShareTheirCommonConjunctsSoTheJoinGetsAKey) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(
        env.Explain(
            "SELECT big.k FROM big, small WHERE (big.k = small.k AND big.v = 1 AND small.z = 'a') "
            "OR (big.k = small.k AND big.v = 2 AND small.z = 'b')"),
        "PROJECT [big.k AS k]\n"
        "  JOIN INNER ON ((big.k = small.k) AND (((v = 1) AND (z = 'a')) OR ((v = 2) AND (z = "
        "'b'))))\n"
        "    SCAN big [k, v]\n"
        "    SCAN small [k, z]\n");
}

TEST(OptimizerShapes, InnerJoinTreesAreNotFlattenedAcrossAnOuterJoin) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(
        env.Explain(
            "SELECT big.k FROM big LEFT JOIN mid ON big.k = mid.k, small WHERE small.k = big.v"),
        "PROJECT [big.k AS k]\n"
        "  JOIN INNER ON (small.k = v)\n"
        "    JOIN LEFT ON (big.k = mid.k)\n"
        "      SCAN big [k, v]\n"
        "      SCAN mid [k]\n"
        "    SCAN small [k]\n");
}

// ---------------------------------------------------------------------------------- semantics

namespace {

// Strictly-equal results: same shape, same order, doubles within a relative tolerance.
bool SameResult(const QueryResult& a, const QueryResult& b, bool ordered, std::string& why) {
    if (a.ok() != b.ok()) {
        why = std::string("one failed: ") + (a.ok() ? b.error_message() : a.error_message());
        return false;
    }
    if (!a.ok()) {
        return true; // both failed (the error text may legitimately differ in which row hit it
                     // first)
    }
    if (a.RowCount() != b.RowCount() || a.ColumnCount() != b.ColumnCount()) {
        why = "shape " + std::to_string(a.RowCount()) + "x" + std::to_string(a.ColumnCount()) +
              " vs " + std::to_string(b.RowCount()) + "x" + std::to_string(b.ColumnCount());
        return false;
    }
    auto ra = a.Rows(), rb = b.Rows();
    const auto less = [](const std::vector<Value>& x, const std::vector<Value>& y) {
        for (size_t i = 0; i < x.size(); i++) {
            const bool xn = x[i].IsNull(), yn = y[i].IsNull();
            if (xn || yn) {
                if (xn && yn) {
                    continue;
                }
                return yn; // NULL sorts last
            }
            const int c = Value::Compare(x[i], y[i]);
            if (c != 0) {
                return c < 0;
            }
        }
        return false;
    };
    if (!ordered) {
        std::sort(ra.begin(), ra.end(), less);
        std::sort(rb.begin(), rb.end(), less);
    }
    for (size_t r = 0; r < ra.size(); r++) {
        for (size_t c = 0; c < ra[r].size(); c++) {
            const Value &x = ra[r][c], &y = rb[r][c];
            if (x.IsNull() != y.IsNull()) {
                why = "row " + std::to_string(r) + " col " + std::to_string(c) + " NULL mismatch";
                return false;
            }
            if (x.IsNull()) {
                continue;
            }
            if (x.type().id() == TypeId::Double) {
                const double dx = x.GetDouble(), dy = y.GetDouble();
                if (std::isnan(dx) != std::isnan(dy) ||
                    (!std::isnan(dx) && std::fabs(dx - dy) > 1e-9 * std::max(1.0, std::fabs(dx)))) {
                    why = "row " + std::to_string(r) + " col " + std::to_string(c) + ": " +
                          x.ToString() + " vs " + y.ToString();
                    return false;
                }
            } else if (Value::Compare(x, y) != 0) {
                why = "row " + std::to_string(r) + " col " + std::to_string(c) + ": " +
                      x.ToString() + " vs " + y.ToString();
                return false;
            }
        }
    }
    return true;
}

enum class Kind { Int, Str, Dbl, Date };
struct ColRef {
    std::string name; // qualified
    Kind kind;
};

class QueryGen {
  public:
    explicit QueryGen(Rng& rng) : rng_(rng) {}

    std::string Query() {
        scope_.clear();
        const std::string from = From();
        std::string where;
        const size_t npred = RandBelow(rng_, 4);
        for (size_t i = 0; i < npred; i++) {
            where += (i ? " AND " : " WHERE ") + Pred();
        }
        if (!extra_where_.empty()) {
            where += (where.empty() ? " WHERE " : " AND ") + extra_where_;
        }
        std::string select, tail;
        int outputs;
        if (Chance(rng_, 0.3)) { // aggregate
            const ColRef g = scope_[RandBelow(rng_, scope_.size())];
            const ColRef ic = IntCol();
            select = g.name + ", count(*), count(" + scope_[RandBelow(rng_, scope_.size())].name +
                     "), min(" + scope_[RandBelow(rng_, scope_.size())].name + "), sum(" + ic.name +
                     ")";
            tail = " GROUP BY " + g.name;
            if (Chance(rng_, 0.3)) {
                tail += " HAVING count(*) > " + std::to_string(RandBelow(rng_, 3));
            }
            outputs = 5;
        } else {
            outputs = 1 + static_cast<int>(RandBelow(rng_, 4));
            for (int i = 0; i < outputs; i++) {
                const ColRef c = scope_[RandBelow(rng_, scope_.size())];
                select += (i ? ", " : "") + (Chance(rng_, 0.2) && c.kind == Kind::Int
                                                 ? "coalesce(" + c.name + ", -1) + 1"
                                                 : c.name);
            }
        }
        const bool distinct = Chance(rng_, 0.15);
        std::string sql = std::string("SELECT ") + (distinct ? "DISTINCT " : "") + select +
                          " FROM " + from + where + tail;
        ordered_ = Chance(rng_, 0.6);
        if (ordered_) {
            sql += " ORDER BY ";
            for (int i = 1; i <= outputs; i++) {
                sql += (i > 1 ? ", " : "") + std::to_string(i) + (Chance(rng_, 0.3) ? " DESC" : "");
            }
            if (Chance(rng_, 0.4)) {
                sql += " LIMIT " + std::to_string(RandBelow(rng_, 12));
                if (Chance(rng_, 0.3)) {
                    sql += " OFFSET " + std::to_string(RandBelow(rng_, 6));
                }
            }
        }
        return sql;
    }

    bool ordered() const { return ordered_; }

  private:
    void AddTable(const std::string& alias, const std::vector<std::pair<std::string, Kind>>& cols) {
        for (const auto& [n, k] : cols) {
            scope_.push_back({alias + "." + n, k});
        }
    }
    static std::vector<std::pair<std::string, Kind>> Cols(int table) {
        switch (table) {
        case 1:
            return {{"a", Kind::Int}, {"b", Kind::Str}, {"c", Kind::Dbl}};
        case 2:
            return {{"a", Kind::Int}, {"d", Kind::Int}, {"e", Kind::Str}};
        default:
            return {{"d", Kind::Int}, {"f", Kind::Date}, {"g", Kind::Int}};
        }
    }
    ColRef IntCol() {
        std::vector<ColRef> ints;
        for (const auto& c : scope_) {
            if (c.kind == Kind::Int) {
                ints.push_back(c);
            }
        }
        return ints[RandBelow(rng_, ints.size())];
    }

    std::string From() {
        extra_where_.clear();
        const auto tbl = [&](int t, const std::string& alias) {
            AddTable(alias, Cols(t));
            return "r" + std::to_string(t) + " AS " + alias;
        };
        switch (RandBelow(rng_, 9)) {
        case 0:
            return tbl(1, "x");
        case 1:
            return tbl(1, "x") + ", " + tbl(2, "y");
        case 2:
            return tbl(1, "x") + " JOIN " + tbl(2, "y") + " ON x.a = y.a" + MaybeOn();
        case 3:
            return tbl(1, "x") + " LEFT JOIN " + tbl(2, "y") + " ON x.a = y.a" + MaybeOn();
        case 4:
            return tbl(2, "y") + " RIGHT JOIN " + tbl(1, "x") + " ON x.a = y.a" + MaybeOn();
        case 5: {
            const std::string s = tbl(1, "x") + ", " + tbl(2, "y") + ", " + tbl(3, "z");
            extra_where_ = "x.a = y.a AND y.d = z.d";
            return s;
        }
        case 6: {
            const std::string s = tbl(3, "z") + ", " + tbl(2, "y") + ", " + tbl(1, "x");
            extra_where_ = Chance(rng_, 0.5) ? "x.a = y.a AND y.d = z.d" : "x.a = y.a";
            return s;
        }
        case 7: {
            const std::string s =
                tbl(1, "x") + " LEFT JOIN " + tbl(2, "y") + " ON x.a = y.a, " + tbl(3, "z");
            extra_where_ = Chance(rng_, 0.5) ? "z.d = y.d" : "(z.d = y.d OR z.d = x.a)";
            return s;
        }
        default: {
            AddTable("q", {{"a", Kind::Int}, {"d2", Kind::Int}});
            AddTable("x", Cols(1));
            return std::string("(SELECT a, d + 1 AS d2 FROM r2 WHERE d IS NOT NULL) AS q JOIN ") +
                   "r1 AS x ON q.a = x.a";
        }
        }
    }

    std::string MaybeOn() {
        switch (RandBelow(rng_, 4)) {
        case 0:
            return " AND y.d > " + std::to_string(RandBelow(rng_, 4));
        case 1:
            return " AND x.c < 2.0";
        case 2:
            return " AND (y.e = 'a' OR x.b = 'a')";
        default:
            return "";
        }
    }

    std::string Const(Kind k) {
        switch (k) {
        case Kind::Int:
            return std::to_string(RandBelow(rng_, 6));
        case Kind::Str: {
            static const char* const kStr[] = {"'a'", "'ab'", "'b'", "'ba'"};
            return kStr[RandBelow(rng_, 4)];
        }
        case Kind::Dbl: {
            static const char* const kD[] = {"0.5", "1.5", "2.5"};
            return kD[RandBelow(rng_, 3)];
        }
        case Kind::Date:
            return "DATE '2000-01-0" + std::to_string(1 + RandBelow(rng_, 5)) + "'";
        }
        return "0";
    }

    std::string Pred() {
        const ColRef c = scope_[RandBelow(rng_, scope_.size())];
        static const char* const kOps[] = {"=", "<>", "<", "<=", ">", ">="};
        switch (RandBelow(rng_, 9)) {
        case 0:
        case 1:
            return c.name + " " + kOps[RandBelow(rng_, 6)] + " " + Const(c.kind);
        case 2:
            return c.name + (Chance(rng_, 0.5) ? " IS NULL" : " IS NOT NULL");
        case 3:
            if (c.kind == Kind::Str) {
                return c.name + " LIKE 'a%'";
            }
            return c.name + " IN (" + Const(c.kind) + ", " + Const(c.kind) + ")";
        case 4:
            return "(" + c.name + " = " + Const(c.kind) + " OR " + Pred2() + ")";
        case 5: { // cross-table equality of the same kind
            std::vector<ColRef> same;
            for (const auto& o : scope_) {
                if (o.kind == c.kind && o.name != c.name) {
                    same.push_back(o);
                }
            }
            if (same.empty()) {
                return c.name + " IS NOT NULL";
            }
            return c.name + " = " + same[RandBelow(rng_, same.size())].name;
        }
        case 6: { // non-equi
            std::vector<ColRef> same;
            for (const auto& o : scope_) {
                if (o.kind == c.kind && o.name != c.name) {
                    same.push_back(o);
                }
            }
            if (same.empty()) {
                return c.name + " IS NULL";
            }
            return c.name + " < " + same[RandBelow(rng_, same.size())].name;
        }
        case 7:
            return "NOT (" + c.name + " " + kOps[RandBelow(rng_, 6)] + " " + Const(c.kind) + ")";
        default:
            if (c.kind == Kind::Int) {
                return c.name + " BETWEEN " + Const(c.kind) + " AND " +
                       std::to_string(3 + RandBelow(rng_, 3));
            }
            return c.name + " IS NOT NULL";
        }
    }
    std::string Pred2() {
        const ColRef c = scope_[RandBelow(rng_, scope_.size())];
        return c.name + (Chance(rng_, 0.5) ? " IS NULL" : " > " + Const(c.kind));
    }

    Rng& rng_;
    std::vector<ColRef> scope_;
    std::string extra_where_;
    bool ordered_ = false;
};

void LoadRandomTables(Env& env, Rng& rng) {
    env.Run("CREATE TABLE r1 (a INTEGER, b VARCHAR, c DOUBLE)");
    env.Run("CREATE TABLE r2 (a INTEGER, d INTEGER, e VARCHAR)");
    env.Run("CREATE TABLE r3 (d INTEGER, f DATE, g INTEGER)");
    const auto num = [&]() {
        return Chance(rng, 0.15) ? std::string("NULL") : std::to_string(RandBelow(rng, 6));
    };
    const auto str = [&]() {
        static const char* const kS[] = {"'a'", "'ab'", "'b'", "'ba'", "NULL"};
        return std::string(kS[RandBelow(rng, 5)]);
    };
    const auto dbl = [&]() {
        static const char* const kD[] = {"0.5", "1.5", "2.5", "NULL"};
        return std::string(kD[RandBelow(rng, 4)]);
    };
    const auto date = [&]() {
        return Chance(rng, 0.15) ? std::string("NULL")
                                 : "DATE '2000-01-0" + std::to_string(1 + RandBelow(rng, 5)) + "'";
    };
    std::string v1, v2, v3;
    for (int i = 0; i < 40; i++) {
        v1 += (i ? "," : "") + std::string("(") + num() + "," + str() + "," + dbl() + ")";
    }
    for (int i = 0; i < 30; i++) {
        v2 += (i ? "," : "") + std::string("(") + num() + "," + num() + "," + str() + ")";
    }
    for (int i = 0; i < 15; i++) {
        v3 += (i ? "," : "") + std::string("(") + num() + "," + date() + "," + num() + ")";
    }
    env.Run("INSERT INTO r1 VALUES " + v1);
    env.Run("INSERT INTO r2 VALUES " + v2);
    env.Run("INSERT INTO r3 VALUES " + v3);
}

} // namespace

class OptimizerEquivalence : public ::testing::TestWithParam<uint64_t> {};

TEST_P(OptimizerEquivalence, RandomQueriesGiveTheSameAnswerWithAndWithoutTheOptimizer) {
    Rng rng(GetParam());
    Env env;
    LoadRandomTables(env, rng);
    QueryGen gen(rng);
    int ok_queries = 0, nonempty = 0;
    constexpr int kQueries = 600;
    for (int i = 0; i < kQueries; i++) {
        const std::string sql = gen.Query();
        env.conn.SetOptimizerEnabled(false);
        const QueryResult plain = env.conn.Query(sql);
        env.conn.SetOptimizerEnabled(true);
        const QueryResult optimized = env.conn.Query(sql);
        std::string why;
        ASSERT_TRUE(SameResult(plain, optimized, gen.ordered(), why)) << sql << "\n  " << why;
        if (!plain.ok() && std::getenv("CDB_SHOW_FAILED")) {
            std::cerr << "FAILED: " << sql << "\n   " << plain.error_message().substr(0, 150)
                      << "\n";
        }
        if (plain.ok()) {
            ok_queries++;
            nonempty += plain.RowCount() > 0 ? 1 : 0;
        }
    }
    EXPECT_GT(ok_queries, kQueries * 9 / 10) << "the generator should produce valid queries";
    EXPECT_GT(nonempty, kQueries / 3)
        << "and most should return rows, or the comparison proves little";
}

INSTANTIATE_TEST_SUITE_P(Seeds, OptimizerEquivalence, ::testing::Values(1, 2, 3, 4, 5, 6));

} // namespace cdb
