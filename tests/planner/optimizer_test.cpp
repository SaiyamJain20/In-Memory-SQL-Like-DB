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
#include <regex>
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

    // The plan's shape: EXPLAIN without the row estimate that follows each operator
    // (`  (~123 rows)`), which the explain tests check on their own.
    std::string Explain(const std::string& sql) {
        const QueryResult r = conn.Query("EXPLAIN " + sql);
        EXPECT_TRUE(r.ok()) << r.error_message();
        static const std::regex estimate(R"(  \(~[0-9]+ rows\)$)");
        std::string out;
        for (idx_t i = 0; i < r.RowCount(); i++) {
            const std::string line = r.GetValue(0, i).GetVarchar();
            EXPECT_TRUE(std::regex_search(line, estimate)) << "no estimate on: " << line;
            out += std::regex_replace(line, estimate, "") + "\n";
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
    // Written as small, big, mid: big probes, and what it probes is mid joined with small first -
    // small keeps only 5 of mid's 100 keys, so big meets a 5-row hash table instead of building
    // mid's 100 rows - never a cross join. (The earlier left-deep order, big x mid x small, was
    // the best a greedy search could do; the search is now over all bushy trees by estimated cost.)
    EXPECT_EQ(
        env.Explain("SELECT small.z FROM small, big, mid WHERE small.k = mid.k AND mid.k = big.k"),
        "PROJECT [z]\n"
        "  PROJECT [z]\n"
        "    JOIN INNER ON (mid.k = big.k)\n"
        "      SCAN big [k]\n"
        "      JOIN INNER ON (small.k = mid.k)\n"
        "        SCAN mid [k]\n"
        "        SCAN small [k, z]\n");
    // Listed as big, mid, small the leaves come out in a different order than written, so a
    // projection restores the columns - but the join tree is the same.
    EXPECT_EQ(
        env.Explain("SELECT small.z FROM big, mid, small WHERE small.k = mid.k AND mid.k = big.k"),
        "PROJECT [z]\n"
        "  JOIN INNER ON (mid.k = big.k)\n"
        "    SCAN big [k]\n"
        "    JOIN INNER ON (small.k = mid.k)\n"
        "      SCAN mid [k]\n"
        "      SCAN small [k, z]\n");
}

TEST(OptimizerShapes, JoinOrderWeighsPredicateSelectivityNotJustRelationSize) {
    // The TPC-H Q5 trap. `cust` is the smaller relation, but its only link to `fact` before `ords`
    // joins is `cust.nat = fact.nat`, a 25-valued key: each fact row would match ~4 customers, so
    // the intermediate result quadruples. `ords` is bigger but joins on a unique key and keeps the
    // result at 2000 rows, so it must come first; cust then joins on both of its keys. The search
    // now finds something better still: ords x cust first (500 rows, one customer per order), then
    // fact against that on (ord, nat) together. Either way cust is never joined to fact on `nat`
    // alone.
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
        "  JOIN INNER ON ((fact.ord = ords.ord) AND (cust.nat = fact.nat))\n"
        "    SCAN fact [id, ord, nat]\n"
        "    JOIN INNER ON (ords.cust = cust.cust)\n"
        "      SCAN ords [ord, cust]\n"
        "      SCAN cust [cust, nat]\n");
}

TEST(OptimizerShapes, ACompositeKeyIsSizedAsOneKeyNotAsEachColumnAlone) {
    // li and ps both hold every (pk, sk) pair, so li JOIN ps on BOTH columns returns ~2000 rows.
    // Each column alone has few distinct values (100 and 20); sized per column the join would look
    // like a 100x blow-up and `other` (pk only, ~10,000 rows) would be joined first. The composite
    // key's distinct combinations are capped by the input size, which gives the right order.
    Env env;
    env.Run("CREATE TABLE li (pk INTEGER, sk INTEGER, v INTEGER)");
    env.Run("CREATE TABLE ps (pk INTEGER, sk INTEGER)");
    env.Run("CREATE TABLE other (pk INTEGER)");
    std::string li, ps, other;
    for (int i = 0; i < 2000; i++) {
        li += (i ? "," : "") + std::string("(") + std::to_string(i % 100) + "," +
              std::to_string(i / 100) + "," + std::to_string(i) + ")";
        ps += (i ? "," : "") + std::string("(") + std::to_string(i % 100) + "," +
              std::to_string(i / 100) + ")";
    }
    for (int i = 0; i < 500; i++) {
        other += (i ? "," : "") + std::string("(") + std::to_string(i % 100) + ")";
    }
    env.Run("INSERT INTO li VALUES " + li);
    env.Run("INSERT INTO ps VALUES " + ps);
    env.Run("INSERT INTO other VALUES " + other);
    EXPECT_EQ(env.Explain("SELECT li.v FROM li, ps, other "
                          "WHERE li.pk = ps.pk AND li.sk = ps.sk AND li.pk = other.pk"),
              "PROJECT [v]\n"
              "  JOIN INNER ON (li.pk = other.pk)\n"
              "    JOIN INNER ON ((li.pk = ps.pk) AND (li.sk = ps.sk))\n"
              "      SCAN li [pk, sk, v]\n"
              "      SCAN ps [pk, sk]\n"
              "    SCAN other [pk]\n");
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

TEST(OptimizerShapes, SemiAndAntiJoinsSinkToTheSideOfTheJoinThatTheyMention) {
    Env env;
    env.SizedTables();
    // only big is mentioned: the semi join filters big before the join with mid
    // (small.k matches 5 of big's 1000 distinct keys: the semi join leaves ~5 rows of big, which
    // then build the hash table that mid probes)
    EXPECT_EQ(env.Explain("SELECT big.v FROM big JOIN mid ON big.k = mid.k WHERE EXISTS "
                          "(SELECT 1 FROM small WHERE small.k = big.k)"),
              "PROJECT [v]\n"
              "  PROJECT [v]\n"
              "    JOIN INNER ON (big.k = mid.k)\n"
              "      SCAN mid [k]\n"
              "      JOIN SEMI ON (big.k = k)\n"
              "        SCAN big [k, v]\n"
              "        SCAN small [k]\n");
    EXPECT_EQ(env.Explain("SELECT big.v FROM big JOIN mid ON big.k = mid.k WHERE big.v NOT IN "
                          "(SELECT k FROM small)"),
              "PROJECT [v]\n"
              "  JOIN INNER ON (big.k = mid.k)\n"
              "    JOIN ANTI (NULL-AWARE) ON (v = k)\n"
              "      SCAN big [k, v]\n"
              "      PROJECT [k]\n"
              "        SCAN small [k]\n"
              "    SCAN mid [k]\n");
    // only mid is mentioned: it sinks into the other input
    EXPECT_EQ(env.Explain("SELECT big.v FROM big JOIN mid ON big.k = mid.k WHERE EXISTS "
                          "(SELECT 1 FROM small WHERE small.k = mid.w)"),
              "PROJECT [v]\n"
              "  JOIN INNER ON (big.k = mid.k)\n"
              "    SCAN big [k, v]\n"
              "    JOIN SEMI ON (w = k)\n"
              "      SCAN mid [k, w]\n"
              "      SCAN small [k]\n");
    // a residual that mentions both inputs has to stay above the join
    EXPECT_EQ(env.Explain("SELECT big.v FROM big JOIN mid ON big.k = mid.k WHERE EXISTS "
                          "(SELECT 1 FROM small WHERE small.k = mid.w AND small.k < big.v)"),
              "PROJECT [v]\n"
              "  JOIN SEMI ON ((w = k) AND (k < v))\n"
              "    JOIN INNER ON (big.k = mid.k)\n"
              "      SCAN big [k, v]\n"
              "      SCAN mid [k, w]\n"
              "    SCAN small [k]\n");
}

TEST(OptimizerShapes, SemiAndAntiJoinsStayAboveTheNullSuppliedSideOfAnOuterJoin) {
    Env env;
    env.SizedTables();
    // the semi join mentions mid, the null-supplied side of the LEFT join: moving it below would
    // keep the NULL-extended rows it rejects
    EXPECT_EQ(env.Explain("SELECT big.v FROM big LEFT JOIN mid ON big.k = mid.k WHERE EXISTS "
                          "(SELECT 1 FROM small WHERE small.k = mid.w)"),
              "PROJECT [v]\n"
              "  JOIN SEMI ON (w = k)\n"
              "    JOIN LEFT ON (big.k = mid.k)\n"
              "      SCAN big [k, v]\n"
              "      SCAN mid [k, w]\n"
              "    SCAN small [k]\n");
    EXPECT_EQ(env.Explain("SELECT big.v FROM big LEFT JOIN mid ON big.k = mid.k WHERE NOT EXISTS "
                          "(SELECT 1 FROM small WHERE small.k = big.v)"),
              "PROJECT [v]\n"
              "  JOIN ANTI ON (v = k)\n"
              "    JOIN LEFT ON (big.k = mid.k)\n"
              "      SCAN big [k, v]\n"
              "      SCAN mid [k]\n"
              "    SCAN small [k]\n");
}

TEST(OptimizerShapes, UncorrelatedSubqueriesBecomeJoinsWithoutKeys) {
    Env env;
    env.SizedTables();
    // an aggregate is one row by construction: no guard
    EXPECT_EQ(env.Explain("SELECT v FROM big WHERE v > (SELECT avg(w) FROM mid)"),
              "PROJECT [v]\n"
              "  JOIN INNER ON (CAST(v AS DOUBLE) > $scalar0)\n"
              "    SCAN big [v]\n"
              "    PROJECT [avg(CAST(w AS DOUBLE)) AS avg(w)]\n"
              "      AGGREGATE groups=[] aggregates=[avg(CAST(w AS DOUBLE))]\n"
              "        SCAN mid [w]\n");
    // anything else could return several rows: guarded
    EXPECT_EQ(env.Explain("SELECT v FROM big WHERE v = (SELECT w FROM mid)"),
              "PROJECT [v]\n"
              "  JOIN INNER ON (v = $scalar0)\n"
              "    SCAN big [v]\n"
              "    SCALAR_GUARD (one row, NULL if none, error if several)\n"
              "      PROJECT [w]\n"
              "        SCAN mid [w]\n");
    // an uncorrelated EXISTS has no keys at all; the build side needs no columns
    EXPECT_EQ(env.Explain("SELECT v FROM big WHERE EXISTS (SELECT 1 FROM small)"),
              "PROJECT [v]\n"
              "  JOIN SEMI\n"
              "    SCAN big [v]\n"
              "    PROJECT []\n"
              "      SCAN small [k]\n");
    EXPECT_EQ(
        env.Explain("SELECT v FROM big WHERE NOT EXISTS (SELECT 1 FROM small WHERE z = 'z1')"),
        "PROJECT [v]\n"
        "  JOIN ANTI\n"
        "    SCAN big [v]\n"
        "    PROJECT []\n"
        "      FILTER (z = 'z1')\n"
        "        SCAN small [z] prune(z = z1)\n");
}

TEST(OptimizerShapes, ACorrelatedScalarAggregateJoinsOnItsGroupedInner) {
    Env env;
    env.SizedTables();
    EXPECT_EQ(
        env.Explain("SELECT v FROM big b WHERE v > (SELECT avg(w) FROM mid WHERE mid.k = b.k)"),
        "PROJECT [v]\n"
        "  FILTER (CAST(v AS DOUBLE) > $scalar0)\n"
        "    JOIN LEFT ON (k = $scalar0_key)\n"
        "      SCAN big [k, v]\n"
        "      PROJECT [#0 AS $key0, avg(CAST(w AS DOUBLE)) AS $value]\n"
        "        AGGREGATE groups=[k] aggregates=[avg(CAST(w AS DOUBLE))]\n"
        "          SCAN mid [k, w]\n");
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
    explicit QueryGen(Rng& rng, bool subqueries = false) : rng_(rng), subqueries_(subqueries) {}

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
        const size_t nsub = subqueries_ ? RandBelow(rng_, 3) : 0;
        for (size_t i = 0; i < nsub; i++) {
            where += (where.empty() ? " WHERE " : " AND ") + SubqueryPred();
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
    // A subquery over r2 (alias s) or r3 (alias t), as an AND-ed conjunct: every shape the binder
    // unnests, correlated on an integer column of the outer query.
    std::string SubqueryPred() {
        const std::string outer = IntCol().name;
        const std::string outer2 = IntCol().name;
        const std::string k = std::to_string(RandBelow(rng_, 5));
        static const char* const kCmp[] = {"=", "<>", "<", "<=", ">", ">="};
        const std::string cmp = kCmp[RandBelow(rng_, 6)];
        switch (RandBelow(rng_, 14)) {
        case 0:
            return outer + " IN (SELECT s.a FROM r2 AS s WHERE s.d > " + k + ")";
        case 1:
            return outer + " NOT IN (SELECT s.d FROM r2 AS s WHERE s.d IS NOT NULL AND s.a > " + k +
                   ")";
        case 2: // NOT IN over a column that holds NULLs
            return outer + " NOT IN (SELECT s.d FROM r2 AS s WHERE s.a < " + k + ")";
        case 3:
            return outer + " IN (SELECT t.d FROM r3 AS t)";
        case 4:
            return "EXISTS (SELECT 1 FROM r2 AS s WHERE s.a = " + outer + ")";
        case 5:
            return "NOT EXISTS (SELECT 1 FROM r2 AS s WHERE s.a = " + outer + " AND s.d " + cmp +
                   " " + k + ")";
        case 6: // residual referencing both sides
            return "EXISTS (SELECT 1 FROM r2 AS s WHERE s.a = " + outer + " AND s.d " + cmp + " " +
                   outer2 + ")";
        case 7:
            return "NOT EXISTS (SELECT 1 FROM r2 AS s WHERE s.d " + cmp + " " + outer + ")";
        case 8: // uncorrelated EXISTS: a constant
            return std::string(Chance(rng_, 0.5) ? "" : "NOT ") +
                   "EXISTS (SELECT 1 FROM r3 AS t WHERE t.g " + cmp + " " + k + ")";
        case 9:
            return outer + " " + cmp + " (SELECT max(s.d) FROM r2 AS s WHERE s.a = " + outer2 + ")";
        case 10:
            return "(SELECT count(*) FROM r2 AS s WHERE s.a = " + outer + ") " + cmp + " " + k;
        case 11:
            return outer + " " + cmp + " (SELECT avg(t.g) FROM r3 AS t)";
        case 12:
            return outer + " IN (SELECT s.d FROM r2 AS s WHERE s.a = " + outer2 + ")";
        default:
            return "EXISTS (SELECT 1 FROM r2 AS s WHERE s.a = " + outer +
                   " AND EXISTS (SELECT 1 "
                   "FROM r3 AS t WHERE t.d = s.d))";
        }
    }

    std::string Pred2() {
        const ColRef c = scope_[RandBelow(rng_, scope_.size())];
        return c.name + (Chance(rng_, 0.5) ? " IS NULL" : " > " + Const(c.kind));
    }

    Rng& rng_;
    bool subqueries_;
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

// ---------------------------------------------------------------------------------- OR factoring

TEST(OptimizerShapes, OrFactoringKeepsTheCommonPartAndDropsImpliedRemainders) {
    Env env;
    env.Run("CREATE TABLE r1 (a INTEGER, b VARCHAR, c DOUBLE)");
    // (a AND b1) OR (a AND b2)  ->  a AND (b1 OR b2)
    EXPECT_EQ(env.Explain("SELECT a FROM r1 WHERE (a = 1 AND b = 'x') OR (a = 1 AND b = 'y')"),
              "PROJECT [a]\n"
              "  FILTER ((a = 1) AND ((b = 'x') OR (b = 'y')))\n"
              "    SCAN r1 [a, b] prune(a = 1)\n");
    // absorption: a OR (a AND b) = a, so the remainder disappears entirely
    EXPECT_EQ(env.Explain("SELECT a FROM r1 WHERE a = 1 OR (a = 1 AND b = 'x')"),
              "PROJECT [a]\n"
              "  FILTER (a = 1)\n"
              "    SCAN r1 [a] prune(a = 1)\n");
    // nothing in common: left exactly as written
    EXPECT_EQ(env.Explain("SELECT a FROM r1 WHERE (a = 1 AND b = 'x') OR (a = 2 AND b = 'y')"),
              "PROJECT [a]\n"
              "  FILTER (((a = 1) AND (b = 'x')) OR ((a = 2) AND (b = 'y')))\n"
              "    SCAN r1 [a, b]\n");
    // a conjunct that is not in EVERY branch must not be factored out
    EXPECT_EQ(env.Explain(
                  "SELECT a FROM r1 WHERE (a = 1 AND b = 'x') OR (a = 1 AND b = 'y') OR (b = 'z')"),
              "PROJECT [a]\n"
              "  FILTER ((((a = 1) AND (b = 'x')) OR ((a = 1) AND (b = 'y'))) OR (b = 'z'))\n"
              "    SCAN r1 [a, b]\n");
}

TEST(OptimizerShapes, AlwaysTrueConjunctsAreDroppedAndAlwaysFalseOnesStay) {
    Env env;
    env.SizedTables();
    // `1 = 1` and `3 BETWEEN 2 AND 6` fold to TRUE: they must not linger as `AND true` in a join
    // condition (a residual to evaluate for every matching pair)
    for (const char* always : {"1 = 1", "3 BETWEEN 2 AND 6", "TRUE", "1 = 1 AND 2 < 3"}) {
        EXPECT_EQ(env.Explain(std::string("SELECT big.v FROM big, mid WHERE big.k = mid.k AND ") +
                              always),
                  env.Explain("SELECT big.v FROM big, mid WHERE big.k = mid.k"))
            << always;
        EXPECT_EQ(
            env.Explain(std::string("SELECT small.z FROM big, mid, small WHERE small.k = mid.k AND "
                                    "mid.k = big.k AND ") +
                        always),
            env.Explain(
                "SELECT small.z FROM big, mid, small WHERE small.k = mid.k AND mid.k = big.k"))
            << always;
        EXPECT_EQ(env.Explain(std::string("SELECT v FROM big WHERE ") + always),
                  env.Explain("SELECT v FROM big"))
            << always;
    }
    // a predicate that is always FALSE is kept: it empties the result (here as part of the join
    // condition, since the join is the first place that has every relation it could mention - none)
    EXPECT_NE(
        env.Explain("SELECT big.v FROM big, mid WHERE big.k = mid.k AND 1 = 2").find("AND false"),
        std::string::npos);
    EXPECT_NE(env.Explain("SELECT v FROM big WHERE 1 = 2").find("FILTER false"), std::string::npos);
}

// The three regimes of the join search - exhaustive (<= 12 relations), greedy left-deep (<= 60) and
// "as written" (more) - must all return what the unoptimized plan returns.
TEST(OptimizerEquivalenceDirected, ManyRelationsInEveryRegimeOfTheJoinSearch) {
    Rng rng(91);
    for (const size_t n : {size_t{3}, size_t{11}, size_t{12}, size_t{13}, size_t{14}, size_t{16}}) {
        Env env;
        std::string from, where;
        for (size_t i = 0; i < n; i++) {
            const std::string t = "m" + std::to_string(i);
            env.Run("CREATE TABLE " + t + " (a INTEGER, b INTEGER)");
            // two rows each: the unoptimized cross product of 16 tables is 65,536 rows
            env.Run("INSERT INTO " + t + " VALUES (" + std::to_string(RandBelow(rng, 2)) + ", " +
                    std::to_string(i) + "), (" + std::to_string(RandBelow(rng, 2)) + ", " +
                    std::to_string(100 + i) + ")");
            from += (i ? ", " : "") + t;
            if (i > 0 && Chance(rng, 0.7)) { // a chain of equalities, with gaps (cross products)
                where += (where.empty() ? "" : " AND ") + std::string("m") + std::to_string(i - 1) +
                         ".a = " + t + ".a";
            }
            if (Chance(rng, 0.2)) {
                where += (where.empty() ? "" : " AND ") + t + ".b < " + std::to_string(50 + i);
            }
        }
        const std::string sql = "SELECT count(*), sum(m0.b), min(m" + std::to_string(n - 1) +
                                ".b) FROM " + from + (where.empty() ? "" : " WHERE " + where);
        env.conn.SetOptimizerEnabled(false);
        const QueryResult plain = env.conn.Query(sql);
        env.conn.SetOptimizerEnabled(true);
        const QueryResult optimized = env.conn.Query(sql);
        ASSERT_TRUE(plain.ok()) << sql << "\n" << plain.error_message();
        std::string why;
        ASSERT_TRUE(SameResult(plain, optimized, /*ordered=*/true, why))
            << "n = " << n << ": " << sql << "\n  " << why;
    }
}

TEST(OptimizerEquivalenceDirected, MoreRelationsThanTheSearchCanOrderStillJoinCorrectly) {
    // 62 relations of one row each (so the unoptimized cross product is one row): past the 60 the
    // search can mask, the plan keeps the order as written and all predicates go on the top join
    Env env;
    std::string from, where;
    constexpr int kRelations = 62;
    for (int i = 0; i < kRelations; i++) {
        const std::string t = "w" + std::to_string(i);
        env.Run("CREATE TABLE " + t + " (a INTEGER, b INTEGER)");
        env.Run("INSERT INTO " + t + " VALUES (7, " + std::to_string(i) + ")");
        from += (i ? ", " : "") + t;
        if (i > 0) {
            where += (where.empty() ? "" : " AND ") + std::string("w") + std::to_string(i - 1) +
                     ".a = " + t + ".a";
        }
    }
    for (const std::string& extra :
         {std::string(""), std::string(" AND w61.b > 60"), std::string(" AND w3.b > 99")}) {
        const std::string sql =
            "SELECT count(*), sum(w0.b + w61.b) FROM " + from + " WHERE " + where + extra;
        env.conn.SetOptimizerEnabled(false);
        const QueryResult plain = env.conn.Query(sql);
        env.conn.SetOptimizerEnabled(true);
        const QueryResult optimized = env.conn.Query(sql);
        ASSERT_TRUE(plain.ok()) << plain.error_message();
        ASSERT_TRUE(optimized.ok()) << optimized.error_message();
        std::string why;
        ASSERT_TRUE(SameResult(plain, optimized, /*ordered=*/true, why)) << extra << ": " << why;
        EXPECT_EQ(optimized.GetValue(0, 0).GetBigInt(), extra == " AND w3.b > 99" ? 0 : 1) << extra;
    }
}

TEST(OptimizerEquivalenceDirected, PredicatesWithoutColumnsPlaceThemselvesOnAnyJoinTree) {
    Rng rng(77);
    Env env;
    LoadRandomTables(env, rng);
    const std::vector<std::string> constants = {
        "1 = 1",       "1 = 2",         "3 BETWEEN 2 AND 6", "NULL IS NULL", "(1 < 2 OR 2 < 1)",
        "NOT (1 = 1)", "2 IN (1, 2, 3)"};
    const std::vector<std::string> froms = {
        "r1 AS x",
        "r1 AS x, r2 AS y WHERE x.a = y.a",
        "r1 AS x JOIN r2 AS y ON x.a = y.a",
        "r1 AS x, r2 AS y, r3 AS z WHERE x.a = y.a AND y.d = z.d",
        "r1 AS x, r2 AS y, r3 AS z",
        "r1 AS x LEFT JOIN r2 AS y ON x.a = y.a, r3 AS z WHERE z.d = y.d",
    };
    for (const std::string& from : froms) {
        const bool has_where = from.find(" WHERE ") != std::string::npos;
        for (const std::string& c : constants) {
            const std::string sql =
                "SELECT x.a, x.b FROM " + from + (has_where ? " AND " : " WHERE ") + c;
            env.conn.SetOptimizerEnabled(false);
            const QueryResult plain = env.conn.Query(sql);
            env.conn.SetOptimizerEnabled(true);
            const QueryResult optimized = env.conn.Query(sql);
            ASSERT_TRUE(plain.ok()) << sql << "\n" << plain.error_message();
            std::string why;
            ASSERT_TRUE(SameResult(plain, optimized, /*ordered=*/false, why))
                << sql << "\n  " << why;
        }
    }
}

TEST(OptimizerEquivalenceDirected, OrFactoringPreservesResultsOnNullData) {
    const std::vector<std::string> predicates = {
        "x.a = 1 OR (x.a = 1 AND x.b = 'a')",
        "(x.a = 1 AND x.b = 'a') OR (x.a = 1 AND x.b = 'b')",
        "(x.a = 1 AND x.b = 'a') OR (x.a = 2 AND x.b = 'a')",
        "(x.a = 1 AND x.c > 1.0) OR (x.a = 1) OR (x.a = 2 AND x.c < 1.0)",
        "(x.a > 1 AND x.b IS NULL) OR (x.a > 1 AND x.b IS NOT NULL)",
        "(x.a > 2 AND x.b = 'a' AND x.c > 0.6) OR (x.a > 2 AND x.b = 'a' AND x.c < 0.4) OR (x.a > "
        "2 AND x.b = 'a')",
        "(x.a = 1 OR x.a = 2) AND ((x.b = 'a' AND x.c > 1.0) OR (x.b = 'a' AND x.c < 1.0))",
        "NOT ((x.a = 1 AND x.b = 'a') OR (x.a = 1 AND x.b = 'b'))",
        "(x.a IS NULL AND x.b = 'a') OR (x.a IS NULL AND x.b = 'b')",
        "(x.a = y.a AND y.d = 1) OR (x.a = y.a AND y.d = 2)",
        "(x.a = y.a AND y.d = 1) OR (x.a = y.a)",
        "(x.a = y.a AND x.b = 'a') OR (x.a = y.a AND y.e = 'a') OR (x.a = y.a AND y.d > 3)",
    };
    for (uint64_t seed = 1; seed <= 4; seed++) {
        Rng rng(seed * 1000);
        Env env;
        LoadRandomTables(env, rng);
        for (const std::string& p : predicates) {
            const bool join = p.find("y.") != std::string::npos;
            const std::string sql = std::string("SELECT x.a, x.b, x.c") +
                                    (join ? ", y.d, y.e" : "") + " FROM r1 AS x" +
                                    (join ? ", r2 AS y" : "") + " WHERE " + p;
            env.conn.SetOptimizerEnabled(false);
            const QueryResult plain = env.conn.Query(sql);
            env.conn.SetOptimizerEnabled(true);
            const QueryResult optimized = env.conn.Query(sql);
            ASSERT_TRUE(plain.ok()) << sql << "\n" << plain.error_message();
            std::string why;
            ASSERT_TRUE(SameResult(plain, optimized, /*ordered=*/false, why))
                << sql << "\n  " << why;
        }
    }
}

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

// The same with subqueries in the WHERE clause: unnesting plus semi / anti join pushdown, ON vs
// OFF.
class OptimizerSubqueryEquivalence : public ::testing::TestWithParam<uint64_t> {};

TEST_P(OptimizerSubqueryEquivalence, RandomSubqueriesGiveTheSameAnswerWithAndWithoutTheOptimizer) {
    Rng rng(GetParam());
    Env env;
    LoadRandomTables(env, rng);
    QueryGen gen(rng, /*subqueries=*/true);
    int ok_queries = 0, nonempty = 0, with_subquery = 0;
    constexpr int kQueries = 500;
    for (int i = 0; i < kQueries; i++) {
        const std::string sql = gen.Query();
        with_subquery += sql.find("SELECT", 8) != std::string::npos ? 1 : 0;
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
    EXPECT_GT(with_subquery, kQueries / 2) << "most queries should contain a subquery";
    EXPECT_GT(ok_queries, kQueries * 9 / 10) << "the generator should produce valid queries";
    EXPECT_GT(nonempty, kQueries / 4)
        << "and many should return rows, or the comparison proves little";
}

INSTANTIATE_TEST_SUITE_P(Seeds, OptimizerSubqueryEquivalence,
                         ::testing::Values(11, 12, 13, 14, 15, 16, 17, 18));

} // namespace cdb
