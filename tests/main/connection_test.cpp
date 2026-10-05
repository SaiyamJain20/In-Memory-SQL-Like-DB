#include "main/connection.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace cdb {

namespace {

struct Session {
    Database db;
    Connection conn{db};
    QueryResult Q(const std::string& sql) { return conn.Query(sql); }
    QueryResult Ok(const std::string& sql) {
        QueryResult r = conn.Query(sql);
        EXPECT_TRUE(r.ok()) << sql << "\n" << r.error_message();
        return r;
    }
    std::shared_ptr<Table> Table_(const std::string& name) { return db.catalog().GetTable(name); }
    std::vector<std::vector<Value>> TableRows(const std::string& name) {
        auto table = Table_(name);
        auto snap = table->Snapshot();
        TableScan scan(snap, test::AllColumns(*snap));
        return test::ScanAll(scan);
    }
};

std::string TempFile(const std::string& name, const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() / ("cdb_conn_test_" + name);
    std::ofstream(path) << content;
    return path.string();
}

} // namespace

// ------------------------------------------------------------------ DDL

TEST(Connection, CreateAndDropTable) {
    Session s;
    EXPECT_TRUE(s.Ok("CREATE TABLE t (id INT NOT NULL, name VARCHAR(10), score DECIMAL(5,2))")
                    .ColumnCount() == 0);
    auto table = s.Table_("t");
    ASSERT_EQ(table->schema().size(), 3u);
    EXPECT_EQ(table->schema()[0].type, LogicalType::Integer());
    EXPECT_TRUE(table->schema()[0].not_null);
    EXPECT_FALSE(table->schema()[1].not_null);
    EXPECT_EQ(table->schema()[2].type, LogicalType::Double());

    QueryResult dup = s.Q("CREATE TABLE T (x INT)");
    EXPECT_FALSE(dup.ok());
    EXPECT_EQ(dup.error_code(), ErrorCode::Catalog);
    EXPECT_NE(dup.error_message().find("already exists"), std::string::npos);
    EXPECT_TRUE(s.Q("CREATE TABLE IF NOT EXISTS t (x INT)").ok());
    EXPECT_EQ(s.Table_("t")->schema().size(), 3u); // the existing table is untouched

    s.Ok("DROP TABLE t");
    EXPECT_EQ(s.db.catalog().TryGetTable("t"), nullptr);
    EXPECT_FALSE(s.Q("DROP TABLE t").ok());
    EXPECT_TRUE(s.Q("DROP TABLE IF EXISTS t").ok());
}

TEST(Connection, DuplicateColumnNamesAreRejected) {
    Session s;
    QueryResult r = s.Q("CREATE TABLE t (a INT, A INT)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::Binder);
    EXPECT_EQ(s.db.catalog().TryGetTable("t"), nullptr);
}

// ------------------------------------------------------------------ INSERT

TEST(Connection, InsertValuesReturnsACountAndStoresRows) {
    Session s;
    s.Ok("CREATE TABLE t (id INT, name VARCHAR, score DOUBLE, born DATE)");
    QueryResult r = s.Ok("INSERT INTO t VALUES (1, 'alice', 1.5, '1990-05-01'), (2, 'bob', NULL, "
                         "date '2000-01-31'), "
                         "(3 + 1, 'x' || 'y', 2 * 3, NULL)");
    ASSERT_EQ(r.RowCount(), 1u);
    EXPECT_EQ(r.names(), std::vector<std::string>{"Count"});
    EXPECT_EQ(r.GetValue(0, 0).GetBigInt(), 3);

    auto rows = s.TableRows("t");
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][2].GetInteger(), 4);
    EXPECT_EQ(rows[1][2].GetVarchar(), "xy");
    EXPECT_TRUE(rows[2][1].IsNull());
    EXPECT_EQ(rows[2][2].GetDouble(), 6.0);
    EXPECT_EQ(rows[3][0].ToString(), "1990-05-01");
    EXPECT_TRUE(rows[3][2].IsNull());
}

TEST(Connection, InsertWithColumnListFillsTheRestWithNull) {
    Session s;
    s.Ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    s.Ok("INSERT INTO t (c, a) VALUES (30, 10)");
    s.Ok("INSERT INTO t (b) VALUES ('only b')");
    auto rows = s.TableRows("t");
    EXPECT_EQ(rows[0][0].GetInteger(), 10);
    EXPECT_TRUE(rows[1][0].IsNull());
    EXPECT_EQ(rows[2][0].GetInteger(), 30);
    EXPECT_EQ(rows[1][1].GetVarchar(), "only b");
}

TEST(Connection, InsertAppliesAssignmentCasts) {
    Session s;
    s.Ok("CREATE TABLE t (i INT, d DOUBLE, s VARCHAR, dt DATE, b BOOLEAN, l BIGINT)");
    s.Ok("INSERT INTO t VALUES ('42', 7, 123, '2020-02-29', 1, 5)");
    auto rows = s.TableRows("t");
    EXPECT_EQ(rows[0][0].GetInteger(), 42);
    EXPECT_EQ(rows[1][0].GetDouble(), 7.0);
    EXPECT_EQ(rows[2][0].GetVarchar(), "123");
    EXPECT_EQ(rows[3][0].ToString(), "2020-02-29");
    EXPECT_EQ(rows[4][0].GetBoolean(), true);
    EXPECT_EQ(rows[5][0].GetBigInt(), 5);
    QueryResult bad = s.Q("INSERT INTO t (i) VALUES ('forty-two')");
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.error_code(), ErrorCode::Type);
    EXPECT_NE(bad.error_message().find("LINE 1:"), std::string::npos); // rendered with context
    EXPECT_NE(bad.error_message().find("^"), std::string::npos);
}

TEST(Connection, FailedInsertsLeaveTheTableUntouched) {
    Session s;
    s.Ok("CREATE TABLE t (id INT NOT NULL, v INT)");
    s.Ok("INSERT INTO t VALUES (1, 1)");
    // NOT NULL violation on the third row: nothing from this statement may be stored
    QueryResult r = s.Q("INSERT INTO t VALUES (2, 2), (3, 3), (NULL, 4)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::Execution);
    EXPECT_NE(r.error_message().find("NOT NULL"), std::string::npos);
    EXPECT_EQ(s.Table_("t")->RowCount(), 1u);
    // an evaluation error (overflow) halfway through is equally atomic
    r = s.Q("INSERT INTO t VALUES (10, 1), (2147483647 + 1, 2)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(s.Table_("t")->RowCount(), 1u);
    s.Ok("INSERT INTO t VALUES (5, 5)");
    EXPECT_EQ(s.Table_("t")->RowCount(), 2u);
}

TEST(Connection, LargeMultiRowInsertSpansChunks) {
    Session s;
    s.Ok("CREATE TABLE t (n INT, label VARCHAR)");
    std::string sql = "INSERT INTO t VALUES ";
    const int kRows = 5000; // more than two vectors
    for (int i = 0; i < kRows; i++) {
        sql += (i ? ", (" : "(") + std::to_string(i) + ", 'row number " + std::to_string(i) + "')";
    }
    QueryResult r = s.Ok(sql);
    EXPECT_EQ(r.GetValue(0, 0).GetBigInt(), kRows);
    auto rows = s.TableRows("t");
    ASSERT_EQ(rows[0].size(), static_cast<size_t>(kRows));
    for (int i = 0; i < kRows; i += 997) {
        EXPECT_EQ(rows[0][static_cast<size_t>(i)].GetInteger(), i);
        EXPECT_EQ(rows[1][static_cast<size_t>(i)].GetVarchar(), "row number " + std::to_string(i));
    }
}

TEST(Connection, InsertSelectNeedsTheExecutor) {
    Session s;
    s.Ok("CREATE TABLE t (a INT)");
    QueryResult r = s.Q("INSERT INTO t SELECT a FROM t");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::NotImplemented);
}

// ------------------------------------------------------------------ COPY

TEST(Connection, CopyFromCsvFile) {
    Session s;
    s.Ok("CREATE TABLE people (id INT NOT NULL, name VARCHAR, born DATE)");
    const std::string path = TempFile(
        "people.csv",
        "id|name|born\n1|Ada|1815-12-10\n2|\"Grace \"\"Amazing\"\" Hopper\"|1906-12-09\n3||\n");
    QueryResult r = s.Ok("COPY people FROM '" + path + "' (DELIMITER '|', HEADER)");
    EXPECT_EQ(r.GetValue(0, 0).GetBigInt(), 3);
    auto rows = s.TableRows("people");
    EXPECT_EQ(rows[1][1].GetVarchar(), "Grace \"Amazing\" Hopper");
    EXPECT_TRUE(rows[1][2].IsNull());
    EXPECT_EQ(rows[2][0].ToString(), "1815-12-10");
    std::filesystem::remove(path);
}

TEST(Connection, CopyFailuresAreAtomicAndNameTheProblem) {
    Session s;
    s.Ok("CREATE TABLE t (id INT, v DOUBLE)");
    s.Ok("INSERT INTO t VALUES (0, 0)");
    const std::string path = TempFile("bad.csv", "1,1.5\n2,2.5\nthree,3.5\n");
    QueryResult r = s.Q("COPY t FROM '" + path + "'");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::Execution);
    EXPECT_NE(r.error_message().find("line 3"), std::string::npos) << r.error_message();
    EXPECT_NE(r.error_message().find("column \"id\""), std::string::npos);
    EXPECT_EQ(s.Table_("t")->RowCount(), 1u);
    std::filesystem::remove(path);

    r = s.Q("COPY t FROM '/definitely/not/a/file.csv'");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::Io);
}

// ------------------------------------------------------------------ EXPLAIN / SELECT

TEST(Connection, ExplainReturnsThePlanAsRows) {
    Session s;
    s.Ok("CREATE TABLE t (a INT, b INT)");
    QueryResult r = s.Ok("EXPLAIN SELECT a FROM t WHERE b > 1 ORDER BY a LIMIT 3");
    EXPECT_EQ(r.names(), std::vector<std::string>{"explain_value"});
    ASSERT_EQ(r.RowCount(), 5u);
    EXPECT_EQ(r.GetValue(0, 0).GetVarchar(), "LIMIT 3");
    EXPECT_EQ(r.GetValue(0, 1).GetVarchar(), "  ORDER BY a ASC NULLS LAST");
    EXPECT_EQ(r.GetValue(0, 4).GetVarchar(), "        SCAN t [a, b]");
    QueryResult a = s.Q("EXPLAIN ANALYZE SELECT 1");
    EXPECT_FALSE(a.ok());
    EXPECT_EQ(a.error_code(), ErrorCode::NotImplemented);
}

TEST(Connection, ConstantSelectsRunThroughTheScalarEvaluator) {
    Session s;
    QueryResult r = s.Ok(
        "SELECT 1 + 1 AS two, 'a' || 'b' AS ab, 7 / 2 AS q, 7 % 4 AS m, "
        "date '1998-12-01' - interval '90' day AS d, CASE WHEN 1 < 2 THEN 'y' ELSE 'n' END AS c, "
        "NULL AS n, 2147483647 + 0 AS big");
    ASSERT_EQ(r.RowCount(), 1u);
    EXPECT_EQ(r.names(), (std::vector<std::string>{"two", "ab", "q", "m", "d", "c", "n", "big"}));
    EXPECT_EQ(r.GetValue(0, 0).GetInteger(), 2);
    EXPECT_EQ(r.GetValue(1, 0).GetVarchar(), "ab");
    EXPECT_EQ(r.GetValue(2, 0).GetDouble(), 3.5);
    EXPECT_EQ(r.GetValue(3, 0).GetInteger(), 3);
    EXPECT_EQ(r.GetValue(4, 0).ToString(), "1998-09-02");
    EXPECT_EQ(r.GetValue(5, 0).GetVarchar(), "y");
    EXPECT_TRUE(r.GetValue(6, 0).IsNull());
    EXPECT_EQ(r.GetValue(7, 0).GetInteger(), 2147483647);

    EXPECT_EQ(s.Ok("SELECT 1 LIMIT 0").RowCount(), 0u);
    EXPECT_EQ(s.Ok("SELECT 1 LIMIT 1 OFFSET 1").RowCount(), 0u);
    EXPECT_EQ(s.Ok("SELECT 1 LIMIT 5").RowCount(), 1u);

    QueryResult overflow = s.Q("SELECT 2147483647 + 1");
    EXPECT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.error_code(), ErrorCode::Execution);
    EXPECT_FALSE(s.Q("SELECT CAST('x' AS INTEGER)").ok());
}

TEST(Connection, QueriesOverTablesAreBoundButNotYetExecutable) {
    Session s;
    s.Ok("CREATE TABLE t (a INT)");
    QueryResult r = s.Q("SELECT a FROM t");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::NotImplemented);
    EXPECT_NE(r.error_message().find("Phase 4"), std::string::npos);
}

// ------------------------------------------------------------------ errors and scripts

TEST(Connection, ErrorsAreRenderedWithLineAndCaret) {
    Session s;
    QueryResult r = s.Q("SELECT nope");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error_code(), ErrorCode::Binder);
    EXPECT_EQ(r.error_message(),
              "Binder Error: Referenced column \"nope\" not found in FROM clause\n"
              "LINE 1: SELECT nope\n"
              "               ^");
    r = s.Q("SELECT 1 FROM");
    EXPECT_EQ(r.error_code(), ErrorCode::Syntax);
    EXPECT_NE(r.error_message().find("end of input"), std::string::npos);
    EXPECT_EQ(s.Q("").error_code(), ErrorCode::Syntax);
    EXPECT_EQ(s.Q("  -- only a comment\n").error_code(), ErrorCode::Syntax);
}

TEST(Connection, ScriptsRunStatementsInOrderAndStopAtTheFirstError) {
    Session s;
    auto results =
        s.conn.QueryAll("CREATE TABLE t (a INT); INSERT INTO t VALUES (1), (2); SELECT 40 + 2; "
                        "INSERT INTO t VALUES ('x'); INSERT INTO t VALUES (3)");
    ASSERT_EQ(results.size(), 4u); // the 5th statement never ran
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(results[1].GetValue(0, 0).GetBigInt(), 2);
    EXPECT_EQ(results[2].GetValue(0, 0).GetInteger(), 42);
    EXPECT_FALSE(results[3].ok());
    EXPECT_EQ(s.Table_("t")->RowCount(), 2u);
    // Query() returns the last result
    QueryResult last = s.Q("SELECT 1; SELECT 2");
    EXPECT_EQ(last.GetValue(0, 0).GetInteger(), 2);
}

TEST(Connection, ASyntaxErrorAnywhereMeansNothingRuns) {
    Session s;
    QueryResult r = s.Q("CREATE TABLE t (a INT); SELEC 1");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(s.db.catalog().TryGetTable("t"), nullptr);
}

TEST(Connection, LaterStatementsSeeEarlierDdl) {
    Session s;
    QueryResult r = s.Q("CREATE TABLE t (a INT); EXPLAIN SELECT a FROM t");
    EXPECT_TRUE(r.ok()) << r.error_message();
    EXPECT_EQ(r.GetValue(0, 1).GetVarchar(), "  SCAN t [a]");
}

TEST(Connection, TwoConnectionsShareOneDatabase) {
    Database db;
    Connection a(db), b(db);
    ASSERT_TRUE(a.Query("CREATE TABLE t (x INT)").ok());
    ASSERT_TRUE(b.Query("INSERT INTO t VALUES (7)").ok());
    EXPECT_EQ(db.catalog().GetTable("t")->RowCount(), 1u);
}

// ------------------------------------------------------------------ rendering

TEST(QueryResult, ToStringRendersATable) {
    Session s;
    QueryResult r = s.Ok("SELECT 1 AS id, 'alice' AS name, NULL AS missing");
    EXPECT_EQ(r.ToString(), " id | name  | missing\n"
                            "----+-------+--------\n"
                            " 1  | alice | NULL   \n"
                            "(1 row)");
    EXPECT_EQ(QueryResult::Empty().ToString(), "");
    EXPECT_EQ(QueryResult::Failure(ErrorCode::Internal, "boom").ToString(), "boom");
    EXPECT_EQ(s.Ok("SELECT 'multi\nline'").ToString(),
              " 'multi\nline'\n-------------\n multi...    \n(1 row)");
}

TEST(QueryResult, RowsAndValueAccessAcrossChunks) {
    DataChunk a, b;
    a.Initialize({LogicalType::Integer()}, 4);
    b.Initialize({LogicalType::Integer()}, 4);
    for (idx_t i = 0; i < 3; i++)
        a.SetValue(0, i, Value::Integer(static_cast<int32_t>(i)));
    for (idx_t i = 0; i < 2; i++)
        b.SetValue(0, i, Value::Integer(static_cast<int32_t>(10 + i)));
    a.SetCardinality(3);
    b.SetCardinality(2);
    std::vector<DataChunk> chunks;
    chunks.push_back(std::move(a));
    chunks.push_back(std::move(b));
    QueryResult r = QueryResult::Success({"n"}, {LogicalType::Integer()}, std::move(chunks));
    EXPECT_EQ(r.RowCount(), 5u);
    EXPECT_EQ(r.GetValue(0, 2).GetInteger(), 2);
    EXPECT_EQ(r.GetValue(0, 3).GetInteger(), 10);
    EXPECT_EQ(r.GetValue(0, 4).GetInteger(), 11);
    const auto rows = r.Rows();
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[4][0].GetInteger(), 11);
}

} // namespace cdb
