#include "planner/binder.h"

#include "common/error.h"
#include "main/connection.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cdb {

namespace {

struct Env {
    Database db;
    Connection conn{db};

    Env() {
        Run("CREATE TABLE t (a INTEGER, b BIGINT, c DOUBLE, d DATE, s VARCHAR, f BOOLEAN)");
        Run("CREATE TABLE u (a INTEGER, x VARCHAR)");
        Run("CREATE TABLE nn (id INTEGER NOT NULL, name VARCHAR)");
    }
    void Run(const std::string& sql) {
        QueryResult r = conn.Query(sql);
        ASSERT_TRUE(r.ok()) << r.error_message();
    }
    std::string Plan(const std::string& sql) { return conn.Plan(sql)->ToString(); }

    struct Err {
        ErrorCode code;
        size_t pos;
        std::string message;
    };
    Err Error_(const std::string& sql) {
        try {
            conn.Plan(sql);
        } catch (const Error& e) {
            return {e.code(), e.position().value_or(~size_t{0}), e.what()};
        }
        ADD_FAILURE() << "expected a bind error for: " << sql;
        return {ErrorCode::Internal, 0, ""};
    }
};

// position of the first occurrence of `needle` in `sql`
size_t At(const std::string& sql, const std::string& needle) {
    const size_t p = sql.find(needle);
    EXPECT_NE(p, std::string::npos) << needle << " not in " << sql;
    return p;
}

} // namespace

// ------------------------------------------------------------------ plans

TEST(BinderPlan, FilterProjectOrderLimit) {
    Env env;
    EXPECT_EQ(
        env.Plan(
            "SELECT a, b + 1 AS b1 FROM t WHERE a > 5 AND s LIKE 'x%' ORDER BY b1 DESC LIMIT 10"),
        "LIMIT 10\n"
        "  ORDER BY b1 DESC NULLS LAST\n"
        "    PROJECT [a, (b + 1::BIGINT) AS b1]\n"
        "      FILTER ((a > 5) AND like(s, 'x%'))\n"
        "        SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, AggregationHavingAndOrderByAggregate) {
    Env env;
    EXPECT_EQ(
        env.Plan("SELECT a, sum(c) FROM t GROUP BY a HAVING count(*) > 1 ORDER BY sum(c) DESC"),
        "ORDER BY sum(c) DESC NULLS LAST\n"
        "  PROJECT [a, sum(c)]\n"
        "    FILTER (count(*) > 1::BIGINT)\n"
        "      AGGREGATE groups=[a] aggregates=[sum(c), count(*)]\n"
        "        SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT sum(c) FROM t"), "PROJECT [sum(c)]\n"
                                                "  AGGREGATE groups=[] aggregates=[sum(c)]\n"
                                                "    SCAN t [a, b, c, d, s, f]\n");
    // identical aggregates are computed once
    EXPECT_EQ(env.Plan("SELECT sum(c), sum(c) + 1, count(*) FROM t"),
              "PROJECT [sum(c), (sum(c) + 1.0) AS sum(c) + 1, count(*)]\n"
              "  AGGREGATE groups=[] aggregates=[sum(c), count(*)]\n"
              "    SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, OrderByMayIntroduceAnAggregateNotOtherwiseInTheQuery) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT s FROM t GROUP BY s ORDER BY max(a) DESC"),
              "PROJECT [s]\n"
              "  ORDER BY max(a) DESC NULLS LAST\n"
              "    PROJECT [s, max(a)]\n"
              "      AGGREGATE groups=[s] aggregates=[max(a)]\n"
              "        SCAN t [a, b, c, d, s, f]\n");
    // an aggregate that is also selected is shared, not computed twice
    EXPECT_EQ(env.Plan("SELECT s, max(a) FROM t GROUP BY s ORDER BY max(a)"),
              "ORDER BY max(a) ASC NULLS LAST\n"
              "  PROJECT [s, max(a)]\n"
              "    AGGREGATE groups=[s] aggregates=[max(a)]\n"
              "      SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, Joins) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT t.a, u.x FROM t JOIN u ON t.a = u.a WHERE t.d >= '1998-01-01'"),
              "PROJECT [t.a AS a, x]\n"
              "  FILTER (d >= DATE '1998-01-01')\n"
              "    JOIN INNER ON (t.a = u.a)\n"
              "      SCAN t [a, b, c, d, s, f]\n"
              "      SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("SELECT * FROM t, u WHERE t.a = u.a"), "PROJECT [a, b, c, d, s, f, a, x]\n"
                                                              "  FILTER (t.a = u.a)\n"
                                                              "    JOIN CROSS\n"
                                                              "      SCAN t [a, b, c, d, s, f]\n"
                                                              "      SCAN u [a, x]\n");
    // USING: the duplicate column is merged away from `*` and from bare names
    EXPECT_EQ(env.Plan("SELECT * FROM t JOIN u USING (a)"), "PROJECT [a, b, c, d, s, f, x]\n"
                                                            "  JOIN INNER ON (t.a = u.a)\n"
                                                            "    SCAN t [a, b, c, d, s, f]\n"
                                                            "    SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("SELECT x FROM t LEFT JOIN u ON t.a = u.a"),
              "PROJECT [x]\n"
              "  JOIN LEFT ON (t.a = u.a)\n"
              "    SCAN t [a, b, c, d, s, f]\n"
              "    SCAN u [a, x]\n");
}

TEST(BinderPlan, OrderByForms) {
    Env env;
    // by ordinal and by alias, with LIMIT/OFFSET
    EXPECT_EQ(env.Plan("SELECT s, a * 2 AS dbl FROM t ORDER BY 2 DESC, s LIMIT 5 OFFSET 2"),
              "LIMIT 5 OFFSET 2\n"
              "  ORDER BY dbl DESC NULLS LAST, s ASC NULLS LAST\n"
              "    PROJECT [s, (a * 2) AS dbl]\n"
              "      SCAN t [a, b, c, d, s, f]\n");
    // an ORDER BY column that is not selected becomes a hidden column that is projected away
    EXPECT_EQ(env.Plan("SELECT a FROM t ORDER BY b"), "PROJECT [a]\n"
                                                      "  ORDER BY b ASC NULLS LAST\n"
                                                      "    PROJECT [a, b]\n"
                                                      "      SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT DISTINCT a FROM t ORDER BY a NULLS FIRST"),
              "ORDER BY a ASC NULLS FIRST\n"
              "  DISTINCT\n"
              "    PROJECT [a]\n"
              "      SCAN t [a, b, c, d, s, f]\n");
    // an output name wins over an input column of the same name
    EXPECT_EQ(env.Plan("SELECT -a AS a FROM t ORDER BY a"), "ORDER BY a ASC NULLS LAST\n"
                                                            "  PROJECT [(-a) AS a]\n"
                                                            "    SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, GroupByAliasAndOrdinal) {
    Env env;
    const std::string expect = "ORDER BY k ASC NULLS LAST\n"
                               "  PROJECT [(a + 1) AS k, count(*)]\n"
                               "    AGGREGATE groups=[(a + 1)] aggregates=[count(*)]\n"
                               "      SCAN t [a, b, c, d, s, f]\n";
    EXPECT_EQ(env.Plan("SELECT a + 1 AS k, count(*) FROM t GROUP BY k ORDER BY 1"), expect);
    EXPECT_EQ(env.Plan("SELECT a + 1 AS k, count(*) FROM t GROUP BY 1 ORDER BY k"), expect);
    EXPECT_EQ(env.Plan("SELECT a + 1 AS k, count(*) FROM t GROUP BY a + 1 ORDER BY k"), expect);
}

TEST(BinderPlan, ConstantFoldingAndCoercion) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT 1 + 2 AS x, 'a' || 'b' AS y, -5 AS z, date '1998-12-01' - interval "
                       "'90' day AS w, "
                       "CAST('5' AS INTEGER) + 1 AS v"),
              "PROJECT [3 AS x, 'ab' AS y, -5 AS z, DATE '1998-09-02' AS w, 6 AS v]\n"
              "  VALUES (1 row)\n");
    // numbers widen to the common type; `/` is always floating point
    EXPECT_EQ(env.Plan("SELECT a + b, a / 2, a + c FROM t"),
              "PROJECT [(CAST(a AS BIGINT) + b) AS a + b, (CAST(a AS DOUBLE) / 2.0) AS a / 2, "
              "(CAST(a AS DOUBLE) + c) AS a + c]\n"
              "  SCAN t [a, b, c, d, s, f]\n");
    // a string literal compared with a DATE column becomes a DATE constant
    EXPECT_EQ(env.Plan("SELECT 1 FROM t WHERE d < '1998-12-01'"),
              "PROJECT [1]\n"
              "  FILTER (d < DATE '1998-12-01')\n"
              "    SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, PredicateExpansions) {
    Env env;
    EXPECT_EQ(
        env.Plan(
            "SELECT a FROM t WHERE a BETWEEN 1 AND 3 AND s NOT IN ('a', 'b') AND s NOT LIKE '%z'"),
        "PROJECT [a]\n"
        "  FILTER ((((a >= 1) AND (a <= 3)) AND (s NOT IN ('a', 'b'))) AND (NOT like(s, '%z')))\n"
        "    SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT a FROM t WHERE a NOT BETWEEN 1 AND 3"),
              "PROJECT [a]\n"
              "  FILTER (NOT ((a >= 1) AND (a <= 3)))\n"
              "    SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT CASE a WHEN 1 THEN 'one' ELSE 'many' END AS c FROM t"),
              "PROJECT [CASE WHEN (a = 1) THEN 'one' ELSE 'many' END AS c]\n"
              "  SCAN t [a, b, c, d, s, f]\n");
    // IN list over mixed numeric types uses the common type
    EXPECT_EQ(env.Plan("SELECT a FROM t WHERE a IN (1, 2.5)"),
              "PROJECT [a]\n"
              "  FILTER (CAST(a AS DOUBLE) IN (1.0, 2.5))\n"
              "    SCAN t [a, b, c, d, s, f]\n");
}

TEST(BinderPlan, DerivedTablesAndColumnAliases) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT x FROM (SELECT a, x FROM u WHERE a > 1) AS q (k, x) WHERE k < 10"),
              "PROJECT [x]\n"
              "  FILTER (k < 10)\n"
              "    PROJECT [a AS k, x]\n"
              "      FILTER (a > 1)\n"
              "        SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("SELECT q.n FROM (SELECT count(*) AS n FROM t) q"),
              "PROJECT [n]\n"
              "  PROJECT [count(*) AS n]\n"
              "    AGGREGATE groups=[] aggregates=[count(*)]\n"
              "      SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT y FROM t AS z (y)"),
              "PROJECT [y]\n"
              "  SCAN t [a, b, c, d, s, f]\n"); // SCAN lists the physical columns
}

TEST(BinderPlan, StarForms) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT t.*, u.x FROM t, u"), "PROJECT [a, b, c, d, s, f, x]\n"
                                                     "  JOIN CROSS\n"
                                                     "    SCAN t [a, b, c, d, s, f]\n"
                                                     "    SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("SELECT * FROM u"), "PROJECT [a, x]\n  SCAN u [a, x]\n");
}

TEST(BinderPlan, Statements) {
    Env env;
    EXPECT_EQ(env.Plan("CREATE TABLE IF NOT EXISTS z (k INT NOT NULL, v VARCHAR(5))"),
              "CREATE TABLE IF NOT EXISTS z (k INTEGER NOT NULL, v VARCHAR)\n");
    EXPECT_EQ(env.Plan("DROP TABLE IF EXISTS t"), "DROP TABLE IF EXISTS t\n");
    EXPECT_EQ(env.Plan("INSERT INTO u VALUES (1, 'x'), (2, NULL)"),
              "INSERT INTO u\n  VALUES (2 rows)\n");
    EXPECT_EQ(env.Plan("INSERT INTO t (a, s) SELECT a, x FROM u"),
              "INSERT INTO t\n"
              "  PROJECT [a, NULL AS b, NULL AS c, NULL AS d, x AS s, NULL AS f]\n"
              "    PROJECT [a, x]\n"
              "      SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("COPY u FROM 'f.csv' (DELIMITER '|', HEADER)"),
              "COPY u FROM 'f.csv' (delimiter '|', header true)\n");
    EXPECT_EQ(env.Plan("EXPLAIN SELECT 1"), "EXPLAIN\n  PROJECT [1]\n    VALUES (1 row)\n");
}

TEST(BinderPlan, OutputSchemaOfTheRoot) {
    Env env;
    auto plan = env.conn.Plan("SELECT a AS id, s, c * 2 FROM t");
    EXPECT_EQ(plan->names, (std::vector<std::string>{"id", "s", "c * 2"}));
    EXPECT_EQ(plan->types, (std::vector<LogicalType>{LogicalType::Integer(), LogicalType::Varchar(),
                                                     LogicalType::Double()}));
    auto agg = env.conn.Plan("SELECT count(*), sum(a), avg(a), min(s), max(d) FROM t");
    EXPECT_EQ(agg->types, (std::vector<LogicalType>{LogicalType::BigInt(), LogicalType::BigInt(),
                                                    LogicalType::Double(), LogicalType::Varchar(),
                                                    LogicalType::Date()}));
}

// ------------------------------------------------------------------ names

TEST(BinderNames, UnknownAndAmbiguousReferences) {
    Env env;
    auto e = env.Error_("SELECT nope FROM t");
    EXPECT_EQ(e.code, ErrorCode::Binder);
    EXPECT_EQ(e.pos, 7u);
    EXPECT_NE(e.message.find("\"nope\" not found"), std::string::npos);

    e = env.Error_("SELECT * FROM nope");
    EXPECT_EQ(e.code, ErrorCode::Catalog);
    EXPECT_EQ(e.pos, 14u);

    e = env.Error_("SELECT a FROM t, u");
    EXPECT_EQ(e.code, ErrorCode::Binder);
    EXPECT_EQ(e.pos, 7u);
    EXPECT_NE(e.message.find("Ambiguous"), std::string::npos);
    EXPECT_NE(e.message.find("t.a"), std::string::npos);
    EXPECT_NE(e.message.find("u.a"), std::string::npos);

    e = env.Error_("SELECT z.a FROM t");
    EXPECT_NE(e.message.find("table \"z\""), std::string::npos);
    e = env.Error_("SELECT t.zz FROM t");
    EXPECT_NE(e.message.find("column \"zz\" not found in table \"t\""), std::string::npos);
    e = env.Error_("SELECT x.* FROM t");
    EXPECT_EQ(e.code, ErrorCode::Binder);
}

TEST(BinderNames, CaseInsensitiveAndAliasing) {
    Env env;
    EXPECT_EQ(env.Plan("SELECT T.A, Q.X FROM T JOIN U AS Q ON t.a = q.a"),
              "PROJECT [t.a AS a, x]\n"
              "  JOIN INNER ON (t.a = q.a)\n"
              "    SCAN t [a, b, c, d, s, f]\n"
              "    SCAN u [a, x]\n");
    // after aliasing, the original table name is no longer visible
    EXPECT_EQ(env.Error_("SELECT t.a FROM t AS z").code, ErrorCode::Binder);
    // the same alias twice is an error
    auto e = env.Error_("SELECT 1 FROM t, t");
    EXPECT_NE(e.message.find("duplicate table alias"), std::string::npos);
    EXPECT_NO_THROW(env.Plan("SELECT 1 FROM t AS x, t AS y"));
}

TEST(BinderNames, UsingRules) {
    Env env;
    EXPECT_EQ(env.Error_("SELECT * FROM t JOIN u USING (nope)").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT * FROM t JOIN u USING (s)").code,
              ErrorCode::Binder); // not on both sides
    EXPECT_EQ(env.Error_("SELECT * FROM t FULL JOIN u USING (a)").code, ErrorCode::NotImplemented);
    // qualified access to the merged-away column still works
    EXPECT_NO_THROW(env.Plan("SELECT u.a FROM t JOIN u USING (a)"));
    EXPECT_NO_THROW(env.Plan("SELECT a FROM t JOIN u USING (a)"));
}

// ------------------------------------------------------------------ types

TEST(BinderTypes, MismatchesAreReportedWithPositions) {
    Env env;
    auto e = env.Error_("SELECT 'a' + 1");
    EXPECT_EQ(e.code, ErrorCode::Type);
    EXPECT_EQ(e.pos, At("SELECT 'a' + 1", "+"));
    EXPECT_NE(e.message.find("+(VARCHAR, INTEGER)"), std::string::npos);

    EXPECT_EQ(env.Error_("SELECT a FROM t WHERE a").code, ErrorCode::Type); // WHERE needs BOOLEAN
    EXPECT_EQ(env.Error_("SELECT a FROM t WHERE s").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT NOT 'x'").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT -'x'").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT a LIKE 1 FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT s < a FROM t").code,
              ErrorCode::Type); // VARCHAR column vs INTEGER column
    EXPECT_EQ(env.Error_("SELECT d + d FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT d * 2 FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT f + 1 FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT CASE WHEN a > 1 THEN 1 ELSE 'x' END FROM t").code,
              ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT a FROM t WHERE a IN (1, 'x')").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT CAST(d AS INTEGER) FROM t").code, ErrorCode::Type);
}

TEST(BinderTypes, BadConstantConversionsFailAtBindTime) {
    Env env;
    auto e = env.Error_("SELECT 1 FROM t WHERE d < 'not a date'");
    EXPECT_EQ(e.code, ErrorCode::Type);
    EXPECT_EQ(e.pos, At("SELECT 1 FROM t WHERE d < 'not a date'", "<"));
    EXPECT_NE(e.message.find("not a date"), std::string::npos);
    e = env.Error_("SELECT CAST('abc' AS INTEGER)");
    EXPECT_EQ(e.code, ErrorCode::Type);
    EXPECT_EQ(e.pos, 7u);
}

TEST(BinderTypes, NullHandling) {
    Env env;
    // an untyped NULL adopts its neighbour's type
    EXPECT_EQ(env.Plan("SELECT a + NULL, NULL = s, COALESCE(NULL, d) FROM t"),
              "PROJECT [(a + NULL) AS a + NULL, (NULL = s) AS NULL = s, coalesce(NULL, d)]\n"
              "  SCAN t [a, b, c, d, s, f]\n");
    auto plan =
        env.conn.Plan("SELECT NULL, CASE WHEN TRUE THEN NULL END, a + NULL, NULL + NULL FROM t");
    EXPECT_EQ(plan->types,
              (std::vector<LogicalType>{LogicalType::Integer(), LogicalType::Integer(),
                                        LogicalType::Integer(), LogicalType::BigInt()}));
}

TEST(BinderTypes, IntervalArithmetic) {
    Env env;
    EXPECT_EQ(env.Error_("SELECT d + INTERVAL '1' day FROM t").code,
              ErrorCode::NotImplemented); // non-constant date
    EXPECT_EQ(env.Error_("SELECT date '2020-01-01' + INTERVAL '1' hour").code,
              ErrorCode::NotImplemented);
    EXPECT_EQ(env.Error_("SELECT INTERVAL '1' day").code, ErrorCode::NotImplemented);
    EXPECT_EQ(env.Error_("SELECT 5 + INTERVAL '1' day").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT INTERVAL '1' day - date '2020-01-01'").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT date '2020-01-01' * INTERVAL '1' day").code, ErrorCode::Type);
    EXPECT_NO_THROW(env.Plan("SELECT 1 FROM t WHERE d < date '1998-12-01' - interval '3' month"));
}

// ------------------------------------------------------------------ aggregation rules

TEST(BinderAggregates, MisuseIsRejected) {
    Env env;
    auto e = env.Error_("SELECT a, count(*) FROM t");
    EXPECT_EQ(e.code, ErrorCode::Binder);
    EXPECT_NE(e.message.find("must appear in the GROUP BY clause"), std::string::npos);
    EXPECT_EQ(e.pos, 7u);
    e = env.Error_("SELECT count(*), a FROM t");
    EXPECT_EQ(e.pos, At("SELECT count(*), a FROM t", "a FROM"));

    EXPECT_NE(env.Error_("SELECT a FROM t GROUP BY b").message.find("\"a\" must appear"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT count(*) FROM t WHERE count(*) > 1").message.find("not allowed"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT sum(sum(a)) FROM t").message.find("not allowed"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT a FROM t GROUP BY sum(a)").message.find("not allowed"),
              std::string::npos);
    EXPECT_NE(
        env.Error_("SELECT a FROM t GROUP BY a HAVING b > 1").message.find("\"b\" must appear"),
        std::string::npos);
    EXPECT_NE(env.Error_("SELECT a FROM t ORDER BY sum(a)").message.find("not allowed"),
              std::string::npos);
    EXPECT_EQ(env.Error_("SELECT sum(*) FROM t").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT count(DISTINCT *) FROM t").code,
              ErrorCode::Syntax); // rejected by the parser
    EXPECT_EQ(env.Error_("SELECT sum(s) FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT avg(d) FROM t").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT sum(a, b) FROM t").code, ErrorCode::Binder);
    EXPECT_NE(env.Error_("SELECT * FROM t WHERE sum(a) > 1").message.find("not allowed"),
              std::string::npos);
}

TEST(BinderAggregates, ValidForms) {
    Env env;
    EXPECT_NO_THROW(env.Plan("SELECT a, b, count(*) FROM t GROUP BY a, b"));
    EXPECT_NO_THROW(env.Plan("SELECT a + 1, sum(c) * 2 FROM t GROUP BY a + 1"));
    EXPECT_NO_THROW(env.Plan("SELECT count(*) FROM t HAVING count(*) > 0"));
    EXPECT_NO_THROW(env.Plan("SELECT s FROM t GROUP BY s HAVING min(a) > 1 ORDER BY max(a)"));
    EXPECT_NO_THROW(
        env.Plan("SELECT count(a), count(DISTINCT s), min(d), max(s), avg(b), sum(c) FROM t"));
    // grouping expression reused inside an aggregate argument and outside
    EXPECT_NO_THROW(env.Plan("SELECT a, sum(a) FROM t GROUP BY a"));
}

TEST(BinderOrderBy, ResolutionErrors) {
    Env env;
    EXPECT_NE(env.Error_("SELECT a FROM t ORDER BY 2").message.find("position 2"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT a FROM t ORDER BY 0").message.find("position 0"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT a, a FROM t ORDER BY a").message.find("ambiguous"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT DISTINCT a FROM t ORDER BY b")
                  .message.find("must appear in the select list"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT a FROM t GROUP BY a ORDER BY b")
                  .message.find("must appear in the GROUP BY"),
              std::string::npos);
    EXPECT_NE(env.Error_("SELECT a FROM t GROUP BY 3").message.find("position 3"),
              std::string::npos);
    EXPECT_EQ(env.Error_("SELECT a FROM t ORDER BY nope").code, ErrorCode::Binder);
}

TEST(BinderLimit, MustBeAConstantNonNegativeInteger) {
    Env env;
    EXPECT_EQ(env.Error_("SELECT a FROM t LIMIT -1").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT a FROM t LIMIT a").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT a FROM t LIMIT 'x'").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT a FROM t LIMIT 1.5").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT a FROM t LIMIT 1 OFFSET -2").code, ErrorCode::Binder);
    EXPECT_EQ(env.Plan("SELECT a FROM t LIMIT NULL"),
              "LIMIT ALL\n  PROJECT [a]\n    SCAN t [a, b, c, d, s, f]\n");
    EXPECT_EQ(env.Plan("SELECT a FROM t LIMIT 2 + 3"),
              "LIMIT 5\n  PROJECT [a]\n    SCAN t [a, b, c, d, s, f]\n");
}

// ------------------------------------------------------------------ functions

TEST(BinderFunctions, Resolution) {
    Env env;
    auto e = env.Error_("SELECT nofunc(1)");
    EXPECT_EQ(e.code, ErrorCode::Binder);
    EXPECT_NE(e.message.find("does not exist"), std::string::npos);
    EXPECT_EQ(e.pos, 7u);
    EXPECT_EQ(env.Error_("SELECT abs(1, 2)").code, ErrorCode::Binder);
    EXPECT_NE(env.Error_("SELECT abs(1, 2)").message.find("expects 1 argument(s) but 2"),
              std::string::npos);
    EXPECT_EQ(env.Error_("SELECT substring('a')").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT upper(1)").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT length(5)").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT year('1998-01-01')").code, ErrorCode::Type); // strictly typed
    EXPECT_EQ(env.Error_("SELECT year(NULL)").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT substring('abc', 1.5)").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT abs('x')").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT coalesce()").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT coalesce(1, 'x')").code, ErrorCode::Type);
    EXPECT_EQ(env.Error_("SELECT upper(DISTINCT 'a')").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("SELECT EXTRACT(century FROM date '2020-01-01')").code,
              ErrorCode::NotImplemented);
    EXPECT_EQ(
        env.Plan(
            "SELECT upper(s), length(s), substring(s FROM 2 FOR 3), EXTRACT(year FROM d) FROM t"),
        "PROJECT [upper(s), length(s), substring(s, 2, 3), year(d) AS "
        "EXTRACT(year FROM d)]\n"
        "  SCAN t [a, b, c, d, s, f]\n");
}

// ------------------------------------------------------------------ DML binding

TEST(BinderDml, InsertValidation) {
    Env env;
    EXPECT_EQ(env.Error_("INSERT INTO t VALUES (1)").code,
              ErrorCode::Binder); // 1 value for 6 columns
    EXPECT_EQ(env.Error_("INSERT INTO u (a, nope) VALUES (1, 'x')").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("INSERT INTO u (a, a) VALUES (1, 2)").code, ErrorCode::Binder);
    EXPECT_EQ(env.Error_("INSERT INTO nope VALUES (1)").code, ErrorCode::Catalog);
    EXPECT_EQ(env.Error_("INSERT INTO u SELECT a FROM u").code, ErrorCode::Binder); // arity
    auto e = env.Error_("INSERT INTO u VALUES ('abc', 'x')");
    EXPECT_EQ(e.code, ErrorCode::Type);
    EXPECT_EQ(e.pos, At("INSERT INTO u VALUES ('abc', 'x')", "'abc'"));
    EXPECT_EQ(env.Error_("INSERT INTO u VALUES (1, 2, 3)").code, ErrorCode::Binder);
    EXPECT_NO_THROW(
        env.Plan("INSERT INTO u VALUES (1, 'x'), ('2', NULL), (CAST(3.0 AS INTEGER), 'y' || 'z')"));
    EXPECT_NO_THROW(env.Plan("INSERT INTO u (x) VALUES ('only x')"));
    EXPECT_EQ(env.Error_("INSERT INTO t (d) VALUES (5)").code,
              ErrorCode::Type); // INTEGER cannot become DATE
}

TEST(BinderDml, CopyRequiresAnExistingTable) {
    Env env;
    EXPECT_EQ(env.Error_("COPY nope FROM 'x.csv'").code, ErrorCode::Catalog);
}

// ------------------------------------------------------------------ not implemented

TEST(BinderNotImplemented, UnsupportedSubqueryShapesPointAtTheOffendingExpression) {
    Env env;
    struct Case {
        const char* sql;
        const char* anchor; // the error points here
        const char* fragment;
    };
    const Case cases[] = {
        // a correlated scalar subquery needs a join below the aggregation it would feed
        {"SELECT a, (SELECT max(u.a) FROM u WHERE u.a = t.a) FROM t GROUP BY a", "(SELECT max(u.a)",
         "only supported in the WHERE clause"},
        {"SELECT sum(a), (SELECT max(u.a) FROM u WHERE u.a = t.a) FROM t", "(SELECT max(u.a)",
         "only supported in the WHERE clause"},
        // only equality correlation can become a join key
        {"SELECT a FROM t WHERE a > (SELECT max(u.a) FROM u WHERE u.a < t.a)", "(SELECT max(u.a)",
         "only equality correlation"},
        // a correlated scalar subquery must be one aggregate
        {"SELECT a FROM t WHERE a > (SELECT u.a FROM u WHERE u.a = t.a)", "(SELECT u.a",
         "must be an aggregate"},
        {"SELECT a FROM t WHERE a > (SELECT max(u.a) FROM u WHERE u.a = t.a LIMIT 1)",
         "(SELECT max(u.a)", "without GROUP BY"},
        // IN / EXISTS only as AND-ed WHERE conjuncts
        {"SELECT a FROM t WHERE a IN (SELECT a FROM u) OR b = 1", "IN", "AND-ed conditions"},
        {"SELECT a IN (SELECT a FROM u) FROM t", "IN", "AND-ed conditions"},
        // NOT IN with a correlated subquery needs a per-group null-aware anti join
        {"SELECT a FROM t WHERE a NOT IN (SELECT u.a FROM u WHERE u.x = t.s)", "NOT IN",
         "use NOT EXISTS"},
        // a subquery has no place in a join condition
        {"SELECT t.a FROM t JOIN u ON t.a = (SELECT 1)", "(SELECT 1)", "not supported in this"},
    };
    for (const Case& c : cases) {
        const auto e = env.Error_(c.sql);
        EXPECT_EQ(e.code, ErrorCode::NotImplemented) << c.sql << " -> " << e.message;
        EXPECT_EQ(e.pos, At(c.sql, c.anchor)) << c.sql;
        EXPECT_NE(e.message.find(c.fragment), std::string::npos) << c.sql << " -> " << e.message;
    }
    EXPECT_EQ(env.Error_("WITH RECURSIVE x AS (SELECT 1) SELECT * FROM x").code,
              ErrorCode::NotImplemented);
}

// ------------------------------------------------------------------ WITH and subqueries

TEST(BinderCte, PlansInlineTheirDefinitionAndHonourColumnNames) {
    Env env;
    // (the bound plan, before the optimizer prunes the scan to the columns that are used)
    EXPECT_EQ(env.Plan("WITH x AS (SELECT a FROM u) SELECT * FROM x"), "PROJECT [a]\n"
                                                                       "  PROJECT [a]\n"
                                                                       "    SCAN u [a, x]\n");
    EXPECT_EQ(env.Plan("WITH x(p, q) AS (SELECT a, x FROM u) SELECT p, q FROM x WHERE p > 1"),
              "PROJECT [p, q]\n"
              "  FILTER (p > 1)\n"
              "    PROJECT [a AS p, x AS q]\n"
              "      SCAN u [a, x]\n");
    // fewer names than columns rename only the first ones
    EXPECT_EQ(env.conn.Plan("WITH x(p) AS (SELECT a, x FROM u) SELECT * FROM x")->names,
              (std::vector<std::string>{"p", "x"}));
}

TEST(BinderCte, ScopingAndNameErrors) {
    Env env;
    // inner WITH shadows the outer one and leaves it intact afterwards
    EXPECT_EQ(env.conn
                  .Plan("WITH x AS (SELECT 1 AS v) SELECT (WITH x AS (SELECT 2 AS v) "
                        "SELECT max(v) FROM x) + (SELECT max(v) FROM x)")
                  ->ColumnCount(),
              1u);
    // a CTE shadows a table, and is gone with its statement
    EXPECT_EQ(env.conn.Plan("WITH t AS (SELECT 1 AS only) SELECT * FROM t")->names,
              (std::vector<std::string>{"only"}));
    EXPECT_EQ(env.conn.Plan("SELECT * FROM t")->ColumnCount(), 6u);
    EXPECT_EQ(env.Error_("WITH x AS (SELECT 1) SELECT 1; SELECT * FROM x").code, ErrorCode::Syntax);

    auto e = env.Error_("WITH x(p, q, r) AS (SELECT a, x FROM u) SELECT * FROM x");
    EXPECT_EQ(e.code, ErrorCode::Binder);
    EXPECT_NE(e.message.find("has 2 columns but more column names were given"), std::string::npos)
        << e.message;
    e = env.Error_("WITH x AS (SELECT 1), X AS (SELECT 2) SELECT * FROM x");
    EXPECT_EQ(e.code, ErrorCode::Binder) << "names are case-insensitive: " << e.message;
    EXPECT_NE(e.message.find("specified more than once"), std::string::npos);
    // a CTE does not see the ones defined after it, nor itself
    const std::string fwd = "WITH a AS (SELECT * FROM b), b AS (SELECT 1) SELECT * FROM a";
    e = env.Error_(fwd);
    EXPECT_EQ(e.code, ErrorCode::Catalog);
    EXPECT_EQ(e.pos, At(fwd, "b)"));
    EXPECT_EQ(env.Error_("WITH a AS (SELECT * FROM a) SELECT * FROM a").code, ErrorCode::Catalog);
}

TEST(BinderSubquery, ErrorsAreReportedAtTheRightExpression) {
    Env env;
    struct Case {
        const char* sql;
        const char* anchor;
        ErrorCode code;
        const char* fragment;
    };
    const Case cases[] = {
        {"SELECT a FROM t WHERE a IN (SELECT a, x FROM u)", "IN", ErrorCode::Binder,
         "must return one column, not 2"},
        {"SELECT a FROM t WHERE a IN (SELECT x FROM u)", "IN", ErrorCode::Type, "cannot compare"},
        {"SELECT a FROM t WHERE s IN (SELECT a FROM u)", "IN", ErrorCode::Type, "cannot compare"},
        {"SELECT a FROM t WHERE a = (SELECT a, x FROM u)", "(SELECT", ErrorCode::Binder,
         "must return one column, not 2"},
        {"SELECT a FROM t WHERE EXISTS (SELECT nope FROM u)", "nope", ErrorCode::Binder,
         "Referenced column \"nope\" not found"},
        {"SELECT a FROM t WHERE a = (SELECT a FROM nope)", "nope", ErrorCode::Catalog,
         "does not exist"},
        // a qualified name that is unknown in the subquery is reported there, not in the outer
        // query
        {"SELECT (SELECT u.nope FROM u WHERE u.a = t.a) FROM t", "u.nope", ErrorCode::Binder,
         "Referenced column \"nope\" not found in table \"u\""},
        {"SELECT a FROM t WHERE a > (SELECT max(a) FROM u WHERE u.a = t.nope)", "t.nope",
         ErrorCode::Binder, "not found in table \"t\""},
        {"SELECT a FROM t WHERE a > (SELECT max(u.x) FROM u)", "> (SELECT", ErrorCode::Type,
         "No function matches"},
        {"SELECT a FROM t WHERE a > (SELECT sum(s) FROM t)", "sum(s)", ErrorCode::Type,
         "requires a numeric argument"},
        // two query levels up
        {"SELECT a FROM t WHERE EXISTS (SELECT 1 FROM u WHERE EXISTS (SELECT 1 FROM nn WHERE "
         "nn.id = t.a))",
         "t.a", ErrorCode::NotImplemented, "two query levels up"},
    };
    for (const Case& c : cases) {
        const auto e = env.Error_(c.sql);
        EXPECT_EQ(e.code, c.code) << c.sql << " -> " << e.message;
        EXPECT_EQ(e.pos, At(c.sql, c.anchor)) << c.sql << " -> " << e.message;
        EXPECT_NE(e.message.find(c.fragment), std::string::npos) << c.sql << " -> " << e.message;
    }
}

TEST(BinderSubquery, OuterNamesResolveToTheInnermostBlockFirst) {
    Env env;
    // both tables have `a`: unqualified inside the subquery means the subquery's own table
    const std::string plan =
        env.Plan("SELECT t.a FROM t WHERE EXISTS (SELECT 1 FROM u WHERE a = 1)");
    EXPECT_NE(plan.find("FILTER (a = 1)"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("JOIN SEMI ON"), std::string::npos) << "uncorrelated: no join keys";
    // qualified: the outer column
    EXPECT_NE(env.Plan("SELECT t.a FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.a = t.a)")
                  .find("JOIN SEMI ON (a = a)"),
              std::string::npos);
    // an alias hides the table name
    EXPECT_EQ(
        env.Error_("SELECT a FROM t AS o WHERE EXISTS (SELECT 1 FROM u WHERE u.a = t.a)").code,
        ErrorCode::Binder);
}

// ------------------------------------------------------------------ TPC-H

TEST(BinderTpch, AllTwentyTwoQueriesBind) {
    Env env;
    std::ifstream schema(std::string(CDB_SOURCE_DIR) + "/bench/tpch/schema.sql");
    std::stringstream ss;
    ss << schema.rdbuf();
    for (const QueryResult& r : env.conn.QueryAll(ss.str()))
        ASSERT_TRUE(r.ok()) << r.error_message();
    ASSERT_EQ(env.db.catalog().ListTables().size(), 8u + 3u); // 8 TPC-H tables + t, u, nn

    for (int q = 1; q <= 22; q++) {
        char name[16];
        std::snprintf(name, sizeof(name), "q%02d.sql", q);
        std::ifstream in(std::string(CDB_SOURCE_DIR) + "/bench/tpch/queries/" + name);
        std::stringstream text;
        text << in.rdbuf();
        ASSERT_FALSE(text.str().empty()) << name;
        try {
            auto plan = env.conn.Plan(text.str());
            EXPECT_GT(plan->ColumnCount(), 0u) << name;
        } catch (const Error& e) {
            ADD_FAILURE() << name << ": " << FormatErrorWithContext(text.str(), e);
        }
    }

    // spot checks of the bound output schemas
    auto q1 = env.conn.Plan(
        std::string("SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, "
                    "avg(l_discount) AS avg_disc, count(*) AS count_order FROM lineitem "
                    "GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus"));
    EXPECT_EQ(q1->names, (std::vector<std::string>{"l_returnflag", "l_linestatus", "sum_qty",
                                                   "avg_disc", "count_order"}));
    EXPECT_EQ(q1->types[2], LogicalType::Double());
    EXPECT_EQ(q1->types[4], LogicalType::BigInt());
}

} // namespace cdb
