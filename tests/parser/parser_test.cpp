#include "parser/parser.h"

#include "parser/token.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cdb {

namespace {

std::string Canon(const std::string& sql) {
    return ParseStatement(sql)->ToString();
}

std::string CanonExpr(const std::string& sql) {
    return ParseExpression(sql)->ToString();
}

struct Err {
    ErrorCode code;
    size_t pos;
    std::string message;
};

template <class F> Err ErrorOf(F&& parse, const std::string& what) {
    try {
        parse();
    } catch (const Error& e) {
        return {e.code(), e.position().value_or(~size_t{0}), e.what()};
    }
    ADD_FAILURE() << "expected an error for: " << what;
    return {ErrorCode::Internal, 0, ""};
}

Err StatementError(const std::string& sql) {
    return ErrorOf([&] { ParseStatement(sql); }, sql);
}

template <class T> const T& As(const ParsedExpr& e, ExprKind kind) {
    EXPECT_EQ(e.kind, kind);
    return static_cast<const T&>(e);
}

const SelectStatement& AsSelect(const StatementPtr& s) {
    EXPECT_EQ(s->kind, StatementKind::Select);
    return static_cast<const SelectStatement&>(*s);
}

} // namespace

// ------------------------------------------------------------------ expression precedence

TEST(ParserExpr, PrecedenceAndAssociativity) {
    const std::pair<const char*, const char*> cases[] = {
        {"1 + 2 * 3", "(1 + (2 * 3))"},
        {"1 * 2 + 3", "((1 * 2) + 3)"},
        {"1 - 2 - 3", "((1 - 2) - 3)"},
        {"a / b / c", "((a / b) / c)"},
        {"a % b * c", "((a % b) * c)"},
        {"a || b || c", "((a || b) || c)"},
        {"a + b || c", "((a + b) || c)"},
        {"(1 + 2) * 3", "((1 + 2) * 3)"},
        {"a OR b AND c", "(a OR (b AND c))"},
        {"a AND b OR c", "((a AND b) OR c)"},
        {"a OR b OR c", "((a OR b) OR c)"},
        {"NOT a = b", "(NOT (a = b))"},
        {"NOT a AND b", "((NOT a) AND b)"},
        {"NOT NOT a", "(NOT (NOT a))"},
        {"a = b AND c < d", "((a = b) AND (c < d))"},
        {"a + b = c - d", "((a + b) = (c - d))"},
        {"-a * b", "((-a) * b)"},
        {"a - -b", "(a - (-b))"},
        {"- - a", "(-(-a))"},
        {"+a", "a"},
        {"a + +b", "(a + b)"},
        {"-x::int", "(-CAST(x AS INTEGER))"},
        {"x::int::double", "CAST(CAST(x AS INTEGER) AS DOUBLE)"},
        {"a != b", "(a <> b)"},
        {"a <> b", "(a <> b)"},
        {"a <= b", "(a <= b)"},
        {"a >= b", "(a >= b)"},
    };
    for (const auto& [sql, expect] : cases) {
        EXPECT_EQ(CanonExpr(sql), expect) << sql;
    }
}

TEST(ParserExpr, PredicateForms) {
    const std::pair<const char*, const char*> cases[] = {
        {"a IS NULL", "(a IS NULL)"},
        {"a IS NOT NULL AND b", "((a IS NOT NULL) AND b)"},
        {"a BETWEEN 1 AND 2", "(a BETWEEN 1 AND 2)"},
        {"a BETWEEN 1 AND 2 AND c", "((a BETWEEN 1 AND 2) AND c)"},
        {"a NOT BETWEEN b + 1 AND c * 2", "(a NOT BETWEEN (b + 1) AND (c * 2))"},
        {"a IN (1, 2, 3)", "(a IN (1, 2, 3))"},
        {"a NOT IN (1)", "(a NOT IN (1))"},
        {"a IN (b + 1, 'x')", "(a IN ((b + 1), 'x'))"},
        {"a LIKE 'x%'", "(a LIKE 'x%')"},
        {"a NOT LIKE b || 'z'", "(a NOT LIKE (b || 'z'))"},
        {"a = (SELECT 1)", "(a = (SELECT 1))"},
        {"a IN (SELECT b FROM t)", "(a IN (SELECT b FROM t))"},
        {"a NOT IN (SELECT b FROM t)", "(a NOT IN (SELECT b FROM t))"},
        {"EXISTS (SELECT 1)", "EXISTS (SELECT 1)"},
        {"NOT EXISTS (SELECT 1 FROM t)", "(NOT EXISTS (SELECT 1 FROM t))"},
    };
    for (const auto& [sql, expect] : cases) {
        EXPECT_EQ(CanonExpr(sql), expect) << sql;
    }
}

TEST(ParserExpr, FunctionsCasesCastsAndSpecialForms) {
    const std::pair<const char*, const char*> cases[] = {
        {"f(a, b + 1)", "f(a, (b + 1))"},
        {"f()", "f()"},
        {"COUNT(*)", "count(*)"},
        {"count( * )", "count(*)"},
        {"count(DISTINCT a)", "count(DISTINCT a)"},
        {"sum(a * (1 - b))", "sum((a * (1 - b)))"},
        {"max(min(a))", "max(min(a))"},
        {"CASE WHEN a THEN 1 ELSE 2 END", "CASE WHEN a THEN 1 ELSE 2 END"},
        {"CASE WHEN a THEN 1 WHEN b THEN 2 END", "CASE WHEN a THEN 1 WHEN b THEN 2 END"},
        {"CASE x WHEN 1 THEN 'a' WHEN 2 THEN 'b' ELSE 'c' END",
         "CASE x WHEN 1 THEN 'a' WHEN 2 THEN 'b' ELSE 'c' END"},
        {"CAST(a AS BIGINT)", "CAST(a AS BIGINT)"},
        {"CAST(a AS VARCHAR(10))", "CAST(a AS VARCHAR)"},
        {"CAST(a AS DECIMAL(15, 2))", "CAST(a AS DOUBLE)"},
        {"CAST(a AS DOUBLE PRECISION)", "CAST(a AS DOUBLE)"},
        {"a::date", "CAST(a AS DATE)"},
        {"EXTRACT(YEAR FROM d)", "EXTRACT(year FROM d)"},
        {"extract(month from o_orderdate + 1)", "EXTRACT(month FROM (o_orderdate + 1))"},
        {"substring(s FROM 1 FOR 2)", "substring(s, 1, 2)"},
        {"substring(s from 3)", "substring(s, 3)"},
        {"substring(s, 2, 5)", "substring(s, 2, 5)"},
        {"left(s, 2)", "left(s, 2)"},
        {"RIGHT(s, 2)", "right(s, 2)"},
    };
    for (const auto& [sql, expect] : cases) {
        EXPECT_EQ(CanonExpr(sql), expect) << sql;
    }
}

TEST(ParserExpr, Literals) {
    auto constant = [](const char* sql) {
        ExprPtr e = ParseExpression(sql);
        EXPECT_EQ(e->kind, ExprKind::Constant) << sql;
        return e;
    };
    auto value_of = [&](const char* sql) {
        return static_cast<ConstantExpr&>(*constant(sql)).value;
    };

    EXPECT_EQ(value_of("0"), Value::Integer(0));
    EXPECT_EQ(value_of("2147483647"), Value::Integer(2147483647));
    EXPECT_EQ(value_of("2147483648"), Value::BigInt(2147483648LL)); // promotes to BIGINT
    EXPECT_EQ(value_of("9223372036854775807"), Value::BigInt(9223372036854775807LL));
    EXPECT_EQ(value_of("9223372036854775808").type(), LogicalType::Double()); // too big for BIGINT
    EXPECT_EQ(value_of("1.5"), Value::Double(1.5));
    EXPECT_EQ(value_of(".5"), Value::Double(0.5));
    EXPECT_EQ(value_of("1e3"), Value::Double(1000.0));
    EXPECT_EQ(value_of("2.5E-1"), Value::Double(0.25));
    EXPECT_EQ(value_of("'it''s'"), Value::Varchar("it's"));
    EXPECT_EQ(value_of("''"), Value::Varchar(""));
    EXPECT_EQ(value_of("TRUE"), Value::Boolean(true));
    EXPECT_EQ(value_of("false"), Value::Boolean(false));
    EXPECT_EQ(static_cast<ConstantExpr&>(*constant("NULL")).literal, LiteralKind::Null);
    EXPECT_TRUE(value_of("null").IsNull());
    EXPECT_EQ(value_of("DATE '1998-12-01'"), Value::Date(Date::FromYMD(1998, 12, 1)));
    EXPECT_EQ(value_of("date '1970-01-01'"), Value::Date(date_t{0}));

    // a minus sign is an operator, not part of the literal
    EXPECT_EQ(CanonExpr("-5"), "(-5)");
    EXPECT_EQ(CanonExpr("-2147483648"), "(-2147483648)");
}

TEST(ParserExpr, Intervals) {
    auto interval = [](const char* sql) {
        ExprPtr e = ParseExpression(sql);
        EXPECT_EQ(e->kind, ExprKind::Interval) << sql;
        const auto& i = static_cast<const IntervalExpr&>(*e);
        return std::make_pair(i.amount, i.unit);
    };
    using P = std::pair<int64_t, std::string>;
    EXPECT_EQ(interval("interval '90' day"), (P{90, "day"}));
    EXPECT_EQ(interval("INTERVAL '3 month'"), (P{3, "month"}));
    EXPECT_EQ(interval("interval '1' years"), (P{1, "year"}));
    EXPECT_EQ(interval("interval '2 days'"), (P{2, "day"}));
    EXPECT_EQ(interval("interval '-5' day"), (P{-5, "day"}));
    EXPECT_EQ(interval("interval '1' week"), (P{1, "week"}));
    EXPECT_EQ(CanonExpr("date '1998-12-01' - interval '90' day"),
              "(DATE '1998-12-01' - INTERVAL '90' day)");
}

TEST(ParserExpr, IdentifiersAndQualifiedNames) {
    EXPECT_EQ(CanonExpr("Foo"), "foo");
    EXPECT_EQ(CanonExpr("T.Col"), "t.col");
    EXPECT_EQ(CanonExpr("\"Weird Name\""), "\"Weird Name\"");
    EXPECT_EQ(CanonExpr("t.\"Col\""), "t.\"Col\"");
    EXPECT_EQ(CanonExpr("\"select\""), "\"select\""); // reserved word stays quoted on output
    EXPECT_EQ(CanonExpr("\"a\"\"b\""), "\"a\"\"b\"");
    // contextual words are ordinary column names when not in their special syntax
    for (const char* name : {"date", "interval", "extract", "substring", "first", "last", "year",
                             "analyze", "header", "delimiter", "nulls"}) {
        EXPECT_EQ(CanonExpr(name), name) << name;
        EXPECT_EQ(CanonExpr(std::string("t.") + name), std::string("t.") + name);
    }
}

TEST(ParserExpr, ColumnRefStructureAndPositions) {
    ExprPtr e = ParseExpression("  t.col + 1");
    const auto& add = As<BinaryExpr>(*e, ExprKind::Binary);
    EXPECT_EQ(add.pos, 8u); // the operator
    const auto& ref = As<ColumnRefExpr>(*add.left, ExprKind::ColumnRef);
    EXPECT_EQ(ref.table, "t");
    EXPECT_EQ(ref.column, "col");
    EXPECT_EQ(ref.pos, 2u);
}

// ------------------------------------------------------------------ SELECT

TEST(ParserSelect, FullClauseSet) {
    StatementPtr s = ParseStatement(
        "select distinct a, b+1 as c, t.* from t where a>1 group by a, b having count(*)>1 "
        "order by a desc nulls first, b asc nulls last, c limit 10 offset 5;");
    const SelectStatement& sel = AsSelect(s);
    EXPECT_TRUE(sel.distinct);
    ASSERT_EQ(sel.items.size(), 3u);
    EXPECT_EQ(sel.items[1].alias, "c");
    EXPECT_EQ(sel.items[2].expr->kind, ExprKind::Star);
    ASSERT_TRUE(sel.from != nullptr);
    EXPECT_NE(sel.where, nullptr);
    EXPECT_EQ(sel.group_by.size(), 2u);
    EXPECT_NE(sel.having, nullptr);
    ASSERT_EQ(sel.order_by.size(), 3u);
    EXPECT_TRUE(sel.order_by[0].descending);
    EXPECT_EQ(sel.order_by[0].nulls, NullOrder::First);
    EXPECT_FALSE(sel.order_by[1].descending);
    EXPECT_EQ(sel.order_by[1].nulls, NullOrder::Last);
    EXPECT_EQ(sel.order_by[2].nulls, NullOrder::Default);
    EXPECT_NE(sel.limit, nullptr);
    EXPECT_NE(sel.offset, nullptr);
    EXPECT_EQ(s->ToString(),
              "SELECT DISTINCT a, (b + 1) AS c, t.* FROM t WHERE (a > 1) GROUP BY a, b HAVING "
              "(count(*) > 1) ORDER BY a DESC NULLS FIRST, b NULLS LAST, c LIMIT 10 OFFSET 5");
}

TEST(ParserSelect, SimpleForms) {
    EXPECT_EQ(Canon("SELECT 1"), "SELECT 1");
    EXPECT_EQ(Canon("select 1+1"), "SELECT (1 + 1)");
    EXPECT_EQ(Canon("SELECT * FROM t"), "SELECT * FROM t");
    EXPECT_EQ(Canon("SELECT ALL a FROM t"), "SELECT a FROM t");
    EXPECT_EQ(Canon("SELECT a AS x, b y, c \"Z\", d AS \"two words\" FROM t"),
              "SELECT a AS x, b AS y, c AS \"Z\", d AS \"two words\" FROM t");
    EXPECT_EQ(Canon("SELECT t.*, u.a FROM t, u"), "SELECT t.*, u.a FROM t CROSS JOIN u");
    EXPECT_EQ(Canon("SELECT a FROM t LIMIT 1"), "SELECT a FROM t LIMIT 1");
    EXPECT_EQ(Canon("SELECT a FROM t ORDER BY 1"), "SELECT a FROM t ORDER BY 1");
}

TEST(ParserSelect, ClauseKeywordsMayNotBeBareAliases) {
    // FROM / WHERE / ... are reserved, so `SELECT a FROM t` never treats FROM as an alias
    EXPECT_EQ(Canon("SELECT a FROM t WHERE b"), "SELECT a FROM t WHERE b");
    EXPECT_EQ(StatementError("SELECT a b c FROM t").code, ErrorCode::Syntax);
}

TEST(ParserFrom, JoinsAndAliases) {
    const std::pair<const char*, const char*> cases[] = {
        {"SELECT 1 FROM a, b", "SELECT 1 FROM a CROSS JOIN b"},
        {"SELECT 1 FROM a, b, c", "SELECT 1 FROM a CROSS JOIN b CROSS JOIN c"},
        {"SELECT 1 FROM a JOIN b ON a.x = b.x", "SELECT 1 FROM a INNER JOIN b ON (a.x = b.x)"},
        {"SELECT 1 FROM a INNER JOIN b ON 1", "SELECT 1 FROM a INNER JOIN b ON 1"},
        {"SELECT 1 FROM a LEFT JOIN b ON 1", "SELECT 1 FROM a LEFT JOIN b ON 1"},
        {"SELECT 1 FROM a LEFT OUTER JOIN b ON 1", "SELECT 1 FROM a LEFT JOIN b ON 1"},
        {"SELECT 1 FROM a RIGHT OUTER JOIN b ON 1", "SELECT 1 FROM a RIGHT JOIN b ON 1"},
        {"SELECT 1 FROM a FULL JOIN b ON 1", "SELECT 1 FROM a FULL JOIN b ON 1"},
        {"SELECT 1 FROM a CROSS JOIN b", "SELECT 1 FROM a CROSS JOIN b"},
        {"SELECT 1 FROM a JOIN b USING (k, l)", "SELECT 1 FROM a INNER JOIN b USING (k, l)"},
        {"SELECT 1 FROM a x JOIN b AS y ON x.k = y.k",
         "SELECT 1 FROM a AS x INNER JOIN b AS y ON (x.k = y.k)"},
        {"SELECT 1 FROM a JOIN b ON 1 JOIN c ON 2",
         "SELECT 1 FROM a INNER JOIN b ON 1 INNER JOIN c ON 2"},
        {"SELECT 1 FROM a, b JOIN c ON 1", "SELECT 1 FROM a CROSS JOIN (b INNER JOIN c ON 1)"},
        {"SELECT 1 FROM (a JOIN b ON 1) JOIN c ON 2",
         "SELECT 1 FROM a INNER JOIN b ON 1 INNER JOIN c ON 2"},
        {"SELECT 1 FROM a JOIN (b JOIN c ON 1) ON 2",
         "SELECT 1 FROM a INNER JOIN (b INNER JOIN c ON 1) ON 2"},
        {"SELECT 1 FROM (SELECT 1) AS s", "SELECT 1 FROM (SELECT 1) AS s"},
        {"SELECT 1 FROM (SELECT 1) s", "SELECT 1 FROM (SELECT 1) AS s"},
        {"SELECT 1 FROM (SELECT 1, 2) AS s (x, y)", "SELECT 1 FROM (SELECT 1, 2) AS s (x, y)"},
        {"SELECT 1 FROM t AS x (a, b)", "SELECT 1 FROM t AS x (a, b)"},
    };
    for (const auto& [sql, expect] : cases) {
        EXPECT_EQ(Canon(sql), expect) << sql;
    }
}

TEST(ParserFrom, JoinTreesAreLeftDeep) {
    StatementPtr s = ParseStatement("SELECT 1 FROM a JOIN b ON 1 JOIN c ON 2 CROSS JOIN d");
    const TableRef* ref = AsSelect(s).from.get();
    std::vector<std::string> right_names;
    while (ref->kind == TableRefKind::Join) {
        const auto& j = static_cast<const JoinRef&>(*ref);
        right_names.push_back(static_cast<const BaseTableRef&>(*j.right).name);
        ref = j.left.get();
    }
    EXPECT_EQ(static_cast<const BaseTableRef&>(*ref).name, "a");
    EXPECT_EQ(right_names, (std::vector<std::string>{"d", "c", "b"}));
}

TEST(ParserFrom, JoinRequiresOnOrUsing) {
    EXPECT_EQ(StatementError("SELECT 1 FROM a JOIN b").code, ErrorCode::Syntax);
    EXPECT_NE(StatementError("SELECT 1 FROM a JOIN b WHERE 1").message.find("expected ON or USING"),
              std::string::npos);
}

TEST(ParserFrom, DerivedTableColumnAliasesAreRecorded) {
    StatementPtr s = ParseStatement("SELECT 1 FROM (SELECT 1, 2) AS s (x, y)");
    const auto& sub = static_cast<const SubqueryRef&>(*AsSelect(s).from);
    EXPECT_EQ(sub.alias, "s");
    EXPECT_EQ(sub.column_aliases, (std::vector<std::string>{"x", "y"}));
}

// ------------------------------------------------------------------ DDL / DML

TEST(ParserDdl, CreateTable) {
    EXPECT_EQ(
        Canon("CREATE TABLE t (a INT NOT NULL, b VARCHAR(25), c DECIMAL(15,2), d DATE, "
              "e DOUBLE PRECISION, f BIGINT NULL, g CHAR(1), h BOOLEAN)"),
        "CREATE TABLE t (a INTEGER NOT NULL, b VARCHAR, c DOUBLE, d DATE, e DOUBLE, f BIGINT, "
        "g VARCHAR, h BOOLEAN)");
    EXPECT_EQ(Canon("create table if not exists T (X int)"),
              "CREATE TABLE IF NOT EXISTS t (x INTEGER)");
    EXPECT_EQ(Canon("CREATE TABLE \"My T\" (\"A B\" TEXT)"),
              "CREATE TABLE \"My T\" (\"A B\" VARCHAR)");

    StatementPtr s = ParseStatement("CREATE TABLE t (a INT NOT NULL, b INT)");
    const auto& c = static_cast<const CreateTableStatement&>(*s);
    ASSERT_EQ(c.columns.size(), 2u);
    EXPECT_TRUE(c.columns[0].not_null);
    EXPECT_FALSE(c.columns[1].not_null);
    EXPECT_EQ(c.columns[0].type, LogicalType::Integer());
}

TEST(ParserDdl, DropTable) {
    EXPECT_EQ(Canon("DROP TABLE t"), "DROP TABLE t");
    EXPECT_EQ(Canon("drop table if exists T"), "DROP TABLE IF EXISTS t");
}

TEST(ParserDml, Insert) {
    EXPECT_EQ(Canon("INSERT INTO t VALUES (1, 'a'), (2, NULL)"),
              "INSERT INTO t VALUES (1, 'a'), (2, NULL)");
    EXPECT_EQ(Canon("insert into t (a, b) values (1+1, -2)"),
              "INSERT INTO t (a, b) VALUES ((1 + 1), (-2))");
    EXPECT_EQ(Canon("INSERT INTO t SELECT * FROM u"), "INSERT INTO t SELECT * FROM u");
    EXPECT_EQ(Canon("INSERT INTO t (a) SELECT b FROM u WHERE b > 1"),
              "INSERT INTO t (a) SELECT b FROM u WHERE (b > 1)");
    StatementPtr s = ParseStatement("INSERT INTO t VALUES (1), (2), (3)");
    EXPECT_EQ(static_cast<const InsertStatement&>(*s).rows.size(), 3u);
}

TEST(ParserDml, Copy) {
    EXPECT_EQ(Canon("COPY t FROM 'f.csv'"), "COPY t FROM 'f.csv' (DELIMITER ',', HEADER FALSE)");
    EXPECT_EQ(Canon("COPY t FROM 'f.csv' (DELIMITER '|', HEADER)"),
              "COPY t FROM 'f.csv' (DELIMITER '|', HEADER TRUE)");
    EXPECT_EQ(Canon("COPY t FROM 'f.csv' WITH (FORMAT csv, HEADER false, DELIM ';')"),
              "COPY t FROM 'f.csv' (DELIMITER ';', HEADER FALSE)");
    EXPECT_EQ(Canon("COPY t FROM 'it''s.csv' (HEADER TRUE)"),
              "COPY t FROM 'it''s.csv' (DELIMITER ',', HEADER TRUE)");
}

TEST(ParserDml, Explain) {
    EXPECT_EQ(Canon("EXPLAIN SELECT 1"), "EXPLAIN SELECT 1");
    EXPECT_EQ(Canon("explain analyze select 1"), "EXPLAIN ANALYZE SELECT 1");
    EXPECT_EQ(Canon("EXPLAIN INSERT INTO t VALUES (1)"), "EXPLAIN INSERT INTO t VALUES (1)");
    EXPECT_EQ(StatementError("EXPLAIN EXPLAIN SELECT 1").code, ErrorCode::Syntax);
}

TEST(ParserCheckpoint, IsAStatementButNotAReservedWord) {
    EXPECT_EQ(Canon("CHECKPOINT"), "CHECKPOINT");
    EXPECT_EQ(Canon("checkpoint;"), "CHECKPOINT");
    EXPECT_EQ(Canon("  CheckPoint  ;  "), "CHECKPOINT");
    auto stmts = ParseStatements("CREATE TABLE t (a INTEGER); CHECKPOINT; SELECT 1");
    ASSERT_EQ(stmts.size(), 3u);
    EXPECT_EQ(stmts[1]->kind, StatementKind::Checkpoint);
    // it is a word, not a keyword: a table or column may still be called checkpoint
    EXPECT_EQ(Canon("CREATE TABLE checkpoint (checkpoint INTEGER)"),
              "CREATE TABLE checkpoint (checkpoint INTEGER)");
    EXPECT_EQ(Canon("SELECT checkpoint FROM checkpoint"), "SELECT checkpoint FROM checkpoint");
    // and nothing may follow it
    EXPECT_EQ(StatementError("CHECKPOINT t").code, ErrorCode::Syntax);
    EXPECT_EQ(StatementError("CHECKPOINT; garbage here").code, ErrorCode::Syntax);
}

TEST(ParserScript, MultipleStatements) {
    EXPECT_TRUE(ParseStatements("").empty());
    EXPECT_TRUE(ParseStatements(" ; ;; -- nothing\n").empty());
    auto stmts = ParseStatements("SELECT 1; SELECT 2;; DROP TABLE t");
    ASSERT_EQ(stmts.size(), 3u);
    EXPECT_EQ(stmts[1]->ToString(), "SELECT 2");
    EXPECT_EQ(stmts[2]->kind, StatementKind::DropTable);
    EXPECT_EQ(StatementError("").code, ErrorCode::Syntax); // ParseStatement needs one
    EXPECT_EQ(StatementError("SELECT 1; SELECT 2").code, ErrorCode::Syntax);
    EXPECT_EQ(Canon("SELECT 1;"), "SELECT 1");
    EXPECT_EQ(Canon("  SELECT 1  ;  "), "SELECT 1");
}

// ------------------------------------------------------------------ not implemented

TEST(ParserNotImplemented, ValidButUnsupportedSql) {
    const char* const cases[] = {
        "SELECT 1 UNION SELECT 2",
        "SELECT 1 UNION ALL SELECT 2",
        "SELECT 1 INTERSECT SELECT 2",
        "SELECT 1 EXCEPT SELECT 2",
        "WITH RECURSIVE x AS (SELECT 1) SELECT * FROM x",
        "SELECT * FROM s.t",
        "SELECT s.t.c FROM t",
        "SELECT (1, 2)",
        "CREATE VIEW v AS SELECT 1",
        "CREATE TABLE t (a INT PRIMARY KEY)",
        "CREATE TABLE t (a INT UNIQUE)",
        "CREATE TABLE t (PRIMARY KEY (a))",
        "CREATE TABLE t AS SELECT 1",
        "DROP VIEW v",
        "COPY t TO 'f.csv'",
        "COPY t FROM 'f' (FORMAT parquet)",
    };
    for (const char* sql : cases) {
        const Err e = StatementError(sql);
        EXPECT_EQ(e.code, ErrorCode::NotImplemented) << sql << " -> " << e.message;
        EXPECT_LT(e.pos, std::string(sql).size() + 1) << sql;
    }
}

// ------------------------------------------------------------------ syntax errors

TEST(ParserErrors, PositionsAndMessages) {
    struct Case {
        const char* sql;
        size_t pos;
        const char* fragment;
    };
    const Case cases[] = {
        {"SELECT FROM t", 7, "at or near \"FROM\""},
        {"SELECT 1 +", 10, "at end of input"},
        {"SELECT (1", 9, "expected \")\""},
        {"SELECT 'abc", 7, "unterminated string literal"},
        {"SELECT 1 FROM", 13, "expected a table name"},
        {"SELECT a b c FROM t", 11, "at or near \"c\""},
        {"SELECT * FROM t WHERE", 21, "expected an expression"},
        {"FROM t", 0, "expected a statement"},
        {"SELECT 1 2", 9, "expected end of statement"},
        {"INSERT INTO t VALUES", 20, "expected \"(\""},
        {"INSERT INTO t VALUES (1,", 24, "expected an expression"},
        {"INSERT t VALUES (1)", 7, "expected INTO"},
        {"CREATE TABLE t (a)", 17, "expected a type name"},
        {"CREATE TABLE t (a wibble)", 18, "unknown type \"wibble\""},
        {"CREATE TABLE t (a INT,)", 22, "expected a column name"},
        {"CREATE TABLE t ()", 16, "expected a column name"},
        {"CREATE TABLE (a INT)", 13, "expected a table name"},
        {"SELECT CAST(1 AS)", 16, "expected a type name"},
        {"SELECT CAST(1 INT)", 14, "expected AS"},
        {"SELECT x FROM t WHERE a IN ()", 28, "expected an expression"},
        {"SELECT 1 < 2 < 3", 13, "expected end of statement"},
        {"DROP TABLE", 10, "expected a table name"},
        {"COPY t FROM x", 12, "expected a quoted file path"},
        {"COPY t FROM 'f' (BOGUS)", 17, "expected a COPY option"},
        {"COPY t FROM 'f' (DELIMITER 'ab')", 27, "single character"},
        {"SELECT 1e", 7, "malformed numeric literal"},
        {"SELECT date 'nonsense'", 12, "invalid date literal"},
        {"SELECT date '2023-02-30'", 12, "invalid date literal"},
        {"SELECT interval '3' fortnight", 16, "unknown interval unit"},
        {"SELECT interval 'x' day", 16, "invalid interval amount"},
        {"SELECT interval '3'", 16, "needs a unit"},
        {"SELECT a FROM t ORDER BY a NULLS", 32, "expected FIRST or LAST"},
        {"SELECT CASE END", 12, "expected WHEN"},
        {"SELECT CASE WHEN 1 THEN 2", 25, "expected END"},
        {"SELECT a IS 5", 12, "expected NULL"},
        {"SELECT EXISTS (1)", 15, "expected SELECT"},
        {"SELECT a NOT 5", 9, "expected end of statement"},
        {"SELECT 1 FROM t GROUP a", 22, "expected BY"},
        {"SELECT x BETWEEN 1 2", 19, "expected AND"},
        {"SELECT * FROM (SELECT 1", 23, "expected \")\""},
        {"SELECT 1;;; ) ", 12, "expected a statement"},
        {"WITH SELECT 1", 5, "a name for the common table expression"},
        {"WITH x SELECT 1", 7, "expected AS"},
        {"WITH x AS SELECT 1", 10, "expected \"(\""},
        {"WITH x AS () SELECT 1", 11, "expected SELECT"},
        {"WITH x AS (SELECT 1 SELECT 2", 20, "expected \")\""},
        {"WITH x AS (SELECT 1)", 20, "expected SELECT"},
        {"WITH x AS (SELECT 1),  SELECT 2", 23, "a name for the common table expression"},
        {"WITH x () AS (SELECT 1) SELECT 1", 8, "a column name"},
        {"WITH x (a,) AS (SELECT 1) SELECT 1", 10, "a column name"},
    };
    for (const Case& c : cases) {
        const Err e = StatementError(c.sql);
        EXPECT_EQ(e.code, ErrorCode::Syntax) << c.sql;
        EXPECT_EQ(e.pos, c.pos) << c.sql << " -> " << e.message;
        EXPECT_NE(e.message.find(c.fragment), std::string::npos)
            << c.sql << ": message was \"" << e.message << "\", wanted \"" << c.fragment << "\"";
    }
}

TEST(ParserErrors, ErrorsAreNeverOtherExceptionTypes) {
    // spot check: everything thrown is cdb::Error (the helpers above would not compile-catch
    // anything else, so this guards std::out_of_range from stoll etc.)
    for (const char* sql : {"SELECT 99999999999999999999999999", "SELECT 1e999", "SELECT 1e-999",
                            "SELECT interval '99999999999999999999' day"}) {
        try {
            ParseStatement(sql);
        } catch (const Error&) {
        } catch (...) {
            ADD_FAILURE() << "non-Error exception for " << sql;
        }
    }
}

TEST(ParserErrors, ExtremeNumericLiteralsDoNotThrowOutOfRange) {
    EXPECT_EQ(CanonExpr("99999999999999999999999999"), "1e+26"); // falls back to DOUBLE
    EXPECT_EQ(ErrorOf([] { ParseExpression("1e999"); }, "1e999").code, ErrorCode::Syntax);
}

// ------------------------------------------------------------------ error rendering

TEST(ErrorContext, CaretPointsAtTheOffendingToken) {
    const std::string sql = "SELECT * foo";
    const Err e = StatementError(sql);
    try {
        ParseStatement(sql);
    } catch (const Error& err) {
        EXPECT_EQ(FormatErrorWithContext(sql, err),
                  "Syntax Error: syntax error at or near \"foo\" (expected end of statement)\n"
                  "LINE 1: SELECT * foo\n"
                  "                 ^");
    }
    EXPECT_EQ(e.pos, 9u);
}

TEST(ErrorContext, MultiLineInputShowsTheRightLine) {
    const std::string sql = "SELECT 1\nFROM t\nWHERE";
    try {
        ParseStatement(sql);
        FAIL();
    } catch (const Error& err) {
        EXPECT_EQ(FormatErrorWithContext(sql, err),
                  "Syntax Error: syntax error at end of input (expected an expression)\n"
                  "LINE 3: WHERE\n"
                  "             ^");
    }
    const std::string mid = "SELECT 1\nFROM @t";
    try {
        ParseStatement(mid);
        FAIL();
    } catch (const Error& err) {
        const std::string out = FormatErrorWithContext(mid, err);
        EXPECT_NE(out.find("LINE 2: FROM @t"), std::string::npos) << out;
        EXPECT_EQ(out.substr(out.rfind('\n') + 1), std::string(8 + 5, ' ') + "^");
    }
}

TEST(ErrorContext, PositionZeroAndTabsAndNoPosition) {
    try {
        ParseStatement("@");
        FAIL();
    } catch (const Error& err) {
        EXPECT_EQ(FormatErrorWithContext("@", err),
                  "Syntax Error: unexpected character '@'\nLINE 1: @\n        ^");
    }
    try {
        ParseStatement("SELECT\t@");
        FAIL();
    } catch (const Error& err) {
        EXPECT_NE(FormatErrorWithContext("SELECT\t@", err).find("LINE 1: SELECT @"),
                  std::string::npos);
    }
    EXPECT_EQ(FormatErrorWithContext("x", Error(ErrorCode::Binder, "nope")), "Binder Error: nope");
}

// ------------------------------------------------------------------ limits

TEST(ParserLimits, DeepButReasonableNestingIsAccepted) {
    std::string sql = "SELECT ";
    for (int i = 0; i < 100; i++)
        sql += "(";
    sql += "1";
    for (int i = 0; i < 100; i++)
        sql += ")";
    EXPECT_EQ(Canon(sql), "SELECT 1");

    std::string nested = "SELECT 1";
    for (int i = 0; i < 40; i++)
        nested = "SELECT a FROM (" + nested + ") AS s";
    EXPECT_NO_THROW(ParseStatement(nested));
}

TEST(ParserLimits, ExcessiveNestingIsASyntaxErrorNotAStackOverflow) {
    // (note: "--" would start a comment, so unary-minus chains need spaces)
    for (const char* open : {"(", "NOT ", "- ", "CAST(", "f(", "CASE WHEN ", "+ - "}) {
        std::string sql = "SELECT ";
        for (int i = 0; i < 20000; i++)
            sql += open;
        sql += "1";
        const Err e = StatementError(sql);
        EXPECT_EQ(e.code, ErrorCode::Syntax) << open;
        EXPECT_NE(e.message.find("nested too deeply"), std::string::npos) << open;
    }
    std::string subq = "SELECT ";
    for (int i = 0; i < 5000; i++)
        subq += "(SELECT ";
    EXPECT_EQ(StatementError(subq).code, ErrorCode::Syntax);
}

TEST(ParserLimits, LongLeftDeepChainsAreBoundedByDepth) {
    // 900 terms parse, print and destroy fine; 5000 is rejected rather than overflowing the
    // stack in a recursive destructor or ToString.
    std::string ok = "1";
    for (int i = 0; i < 900; i++)
        ok += " + 1";
    ExprPtr e = ParseExpression(ok);
    EXPECT_GT(e->ToString().size(), 900u * 4);
    EXPECT_LE(e->depth, kMaxExprDepth);

    std::string too_long = "1";
    for (int i = 0; i < 5000; i++)
        too_long += " + 1";
    const Err err = ErrorOf([&] { ParseExpression(too_long); }, "long chain");
    EXPECT_EQ(err.code, ErrorCode::Syntax);
    EXPECT_NE(err.message.find("nested too deeply"), std::string::npos);

    std::string casts = "x";
    for (int i = 0; i < 5000; i++)
        casts += "::int";
    EXPECT_EQ(ErrorOf([&] { ParseExpression(casts); }, "cast chain").code, ErrorCode::Syntax);

    std::string ands = "a";
    for (int i = 0; i < 5000; i++)
        ands += " AND a";
    EXPECT_EQ(ErrorOf([&] { ParseExpression(ands); }, "and chain").code, ErrorCode::Syntax);
}

TEST(ParserLimits, WideInputsAreFine) {
    std::string in = "SELECT 1 WHERE a IN (1";
    for (int i = 2; i <= 20000; i++)
        in += ", " + std::to_string(i);
    in += ")";
    EXPECT_NO_THROW(ParseStatement(in));
    std::string cols = "SELECT c0";
    for (int i = 1; i < 5000; i++)
        cols += ", c" + std::to_string(i);
    cols += " FROM t";
    EXPECT_EQ(AsSelect(ParseStatement(cols)).items.size(), 5000u);
}

// ------------------------------------------------------------------ round trips

namespace {

std::vector<std::string> RoundTripCorpus() {
    std::vector<std::string> c = {
        "SELECT 1",
        "SELECT a, b + 1 AS c FROM t WHERE a > 1 AND (b < 2 OR NOT c) GROUP BY a HAVING sum(b) > 3",
        "SELECT * FROM a JOIN b ON a.x = b.x LEFT JOIN c USING (k) WHERE a.y BETWEEN 1 AND 5",
        "SELECT x FROM a, b, c WHERE a.k = b.k AND b.k = c.k ORDER BY x DESC NULLS LAST LIMIT 5 "
        "OFFSET 2",
        "SELECT CASE WHEN a > 1 THEN 'x' WHEN a < 0 THEN 'y' ELSE 'z' END, CASE a WHEN 1 THEN 2 "
        "END FROM t",
        "SELECT CAST(a AS BIGINT), a::double, EXTRACT(year FROM d), substring(s FROM 2 FOR 3) FROM "
        "t",
        "SELECT a FROM t WHERE a IN (1, 2, 3) AND b NOT IN (SELECT c FROM u) AND EXISTS (SELECT 1)",
        "SELECT a FROM t WHERE s LIKE 'a%' AND s NOT LIKE '%b' AND x IS NOT NULL AND y IS NULL",
        "SELECT (SELECT max(b) FROM u WHERE u.k = t.k) FROM t",
        "SELECT s.x FROM (SELECT a AS x FROM t) AS s (x) WHERE s.x > date '1998-12-01' - interval "
        "'90' day",
        "SELECT \"Weird Col\", \"select\", t.\"A\" FROM \"My Table\" AS t",
        "SELECT 'it''s', 1.5, 2e3, TRUE, NULL, -5, - -5, 9223372036854775807",
        "SELECT count(*), count(DISTINCT a), sum(a * (1 - b)) FROM t",
        "SELECT a || b || 'c' FROM t",
        "SELECT DISTINCT a FROM t",
        "CREATE TABLE t (a INT NOT NULL, b VARCHAR(25), c DECIMAL(15,2), d DATE)",
        "CREATE TABLE IF NOT EXISTS \"T 2\" (x INT)",
        "DROP TABLE IF EXISTS t",
        "INSERT INTO t (a, b) VALUES (1, 'x'), (2, NULL)",
        "INSERT INTO t SELECT a FROM u",
        "COPY t FROM 'it''s.csv' (DELIMITER '|', HEADER)",
        "EXPLAIN ANALYZE SELECT * FROM t",
        "WITH x AS (SELECT 1) SELECT * FROM x",
        "WITH x (a, b) AS (SELECT 1, 2), y AS (SELECT a FROM x) SELECT * FROM x JOIN y ON x.a = "
        "y.a",
        "WITH \"Odd Name\" AS (SELECT 1 AS \"select\") SELECT * FROM \"Odd Name\"",
        "SELECT (WITH x AS (SELECT 1) SELECT max(a) FROM x) FROM t",
        "SELECT a FROM t WHERE a IN (WITH x AS (SELECT 1) SELECT * FROM x)",
        "SELECT * FROM (WITH x AS (SELECT 1 AS a) SELECT a FROM x) AS d",
        "INSERT INTO t WITH x AS (SELECT 1) SELECT * FROM x",
        "WITH x AS (WITH y AS (SELECT 1) SELECT * FROM y) SELECT * FROM x",
        "SELECT a FROM t WHERE NOT EXISTS (SELECT 1 FROM u WHERE u.a = t.a) AND a NOT IN (SELECT b "
        "FROM v)",
    };
    return c;
}

} // namespace

TEST(ParserRoundTrip, PrintedSqlReparsesToTheSameText) {
    for (const std::string& sql : RoundTripCorpus()) {
        const std::string once = Canon(sql);
        const std::string twice = Canon(once);
        EXPECT_EQ(once, twice) << "input: " << sql;
    }
}

TEST(ParserRoundTrip, ReservedWordsAndOddIdentifiersSurvive) {
    // identifiers that need quoting on output must come back as the same identifier
    for (const char* name : {"select", "from", "order", "Mixed Case", "with space", "a\"b", "1abc",
                             "ünï", "null", "left"}) {
        const std::string sql =
            "SELECT " + QuoteIdentifier(name) + " FROM " + QuoteIdentifier(name);
        StatementPtr s = ParseStatement(sql);
        const auto& sel = AsSelect(s);
        EXPECT_EQ(static_cast<const ColumnRefExpr&>(*sel.items[0].expr).column, name) << name;
        EXPECT_EQ(static_cast<const BaseTableRef&>(*sel.from).name, name) << name;
        EXPECT_EQ(Canon(s->ToString()), s->ToString());
    }
}

TEST(ParserRoundTrip, StringLiteralEscapingSurvives) {
    for (const char* text :
         {"", "plain", "it's", "''", "a'b'c", "tab\there", "new\nline", "100%"}) {
        const std::string sql = "SELECT " + QuoteString(text);
        StatementPtr s = ParseStatement(sql);
        const auto& c = static_cast<const ConstantExpr&>(*AsSelect(s).items[0].expr);
        EXPECT_EQ(c.value.GetVarchar(), text);
        EXPECT_EQ(Canon(s->ToString()), s->ToString());
    }
}

TEST(ParserRoundTrip, DoublesSurviveExactly) {
    for (double d : {0.0, 0.1, 0.05, 1.5, 123456.789, 1e-7, 1e20, 5e-324, 1.7976931348623157e308}) {
        const std::string text = Value::Double(d).ToString();
        ExprPtr e = ParseExpression(text);
        ASSERT_EQ(e->kind, ExprKind::Constant) << text;
        EXPECT_EQ(static_cast<const ConstantExpr&>(*e).value.GetDouble(), d) << text;
    }
}

// ------------------------------------------------------------------ TPC-H

namespace {

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(ParserTpch, AllTwentyTwoQueriesParseAndRoundTrip) {
    const std::filesystem::path dir = std::filesystem::path(CDB_SOURCE_DIR) / "bench/tpch/queries";
    std::vector<std::string> not_implemented;
    int parsed = 0;
    for (int q = 1; q <= 22; q++) {
        char name[16];
        std::snprintf(name, sizeof(name), "q%02d.sql", q);
        const std::string sql = ReadFile(dir / name);
        ASSERT_FALSE(sql.empty()) << name;
        try {
            StatementPtr s = ParseStatement(sql);
            ASSERT_EQ(s->kind, StatementKind::Select) << name;
            const std::string once = s->ToString();
            EXPECT_EQ(Canon(once), once) << name << " is not a round-trip fixpoint";
            parsed++;
        } catch (const Error& e) {
            ASSERT_EQ(e.code(), ErrorCode::NotImplemented)
                << name << ": " << FormatErrorWithContext(sql, e);
            not_implemented.push_back(name);
        }
    }
    EXPECT_EQ(parsed, 22);
    EXPECT_TRUE(not_implemented.empty()) << "every TPC-H query parses";
}

} // namespace cdb
