// SQL end to end on a real table: the same queries on 1 and on 4 threads must give the same
// answers, the order guarantees of the parallel executor must hold, and connections may share one
// pool.

#include "execution/hash_aggregate.h"
#include "execution/hash_join.h"
#include "execution/sort.h"
#include "main/connection.h"
#include "main/database.h"
#include "storage/table.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

namespace cdb {

namespace {

// One-vector morsels and every parallel threshold at 1 for the life of the object, so even this
// modest table is cut into many morsels, merged by partition, joined and sorted in parallel.
// `one_vector_morsels` false leaves the morsel size to the scan (built-in default, sized by
// threads).
class ForceParallelPaths {
  public:
    explicit ForceParallelPaths(bool one_vector_morsels = true) {
        MorselScan::SetDefaultMorselRows(one_vector_morsels ? kVectorSize : 0);
        PhysicalHashAggregate::SetMinGroupsToPartition(1);
        PhysicalHashJoin::SetMinRowsToParallelize(1);
        SortBuffer::SetMinRowsToSortInParallel(1);
    }
    ~ForceParallelPaths() {
        MorselScan::SetDefaultMorselRows(0);
        PhysicalHashAggregate::SetMinGroupsToPartition(0);
        PhysicalHashJoin::SetMinRowsToParallelize(0);
        SortBuffer::SetMinRowsToSortInParallel(0);
    }
    ForceParallelPaths(const ForceParallelPaths&) = delete;
    ForceParallelPaths& operator=(const ForceParallelPaths&) = delete;
};

void MustSucceed(Connection& conn, const std::string& sql) {
    const QueryResult r = conn.Query(sql);
    ASSERT_TRUE(r.ok()) << sql << ": " << r.error_message();
}

// 100 rows doubled `rounds` times by INSERT ... SELECT (ten: 102,400 rows) with unique, ascending
// ids. d holds multiples of 0.25 so floating-point sums are exact in any order.
void Populate(Connection& conn, int rounds = 10) {
    MustSucceed(conn, "CREATE TABLE t (id BIGINT, g INTEGER, a INTEGER, s VARCHAR, d DOUBLE)");
    std::string insert = "INSERT INTO t VALUES ";
    for (int i = 0; i < 100; i++) {
        insert += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", " +
                  std::to_string(i % 13) + ", " +
                  (i % 9 == 4 ? "NULL" : std::to_string((i * 7) % 11)) + ", '" +
                  (i % 5 == 0 ? "a fairly long string that is out of line " : "s") +
                  std::to_string((i * 31) % 17) + "', " + std::to_string((i % 40) * 0.25) + ")";
    }
    MustSucceed(conn, insert);
    int64_t rows = 100;
    for (int round = 0; round < rounds; round++) {
        MustSucceed(conn,
                    "INSERT INTO t SELECT id + " + std::to_string(rows) + ", g, a, s, d FROM t");
        rows *= 2;
    }
}

const char* const kQueries[] = {
    "SELECT count(*), sum(a), min(s), max(d), avg(a), count(a), sum(d) FROM t",
    "SELECT g, count(*), sum(a), min(s), max(s), sum(d) FROM t GROUP BY g ORDER BY g",
    "SELECT id % 5000 AS k, count(*), sum(a), sum(d), min(s) FROM t GROUP BY k ORDER BY k",
    "SELECT DISTINCT g, a FROM t ORDER BY g, a",
    "SELECT count(DISTINCT s), count(DISTINCT a) FROM t",
    "SELECT t1.id, t2.s, t1.d FROM t t1 JOIN t t2 ON t1.id = t2.id + 1 WHERE t1.a > 3 "
    "ORDER BY t1.id LIMIT 60",
    "SELECT count(*), sum(t1.a), sum(t2.d) FROM t t1 JOIN t t2 ON t1.id = t2.id + 7 AND t1.g = "
    "t2.g",
    "SELECT count(*) FROM t t1 LEFT JOIN t t2 ON t1.id + 100000 = t2.id AND t2.a > 5",
    "SELECT id, s FROM t ORDER BY s DESC, id LIMIT 1000 OFFSET 17",
    "SELECT id, a, d FROM t ORDER BY a, d DESC, id LIMIT 3000",
    "SELECT g, id FROM t WHERE a > 8 ORDER BY g DESC, id DESC LIMIT 200",
    "SELECT count(*), min(id), max(id) FROM (SELECT id FROM t WHERE a > 5 AND g < 50)",
};

std::vector<int64_t> Ids(Connection& conn, const std::string& sql) {
    const QueryResult r = conn.Query(sql);
    EXPECT_TRUE(r.ok()) << r.error_message();
    std::vector<int64_t> ids;
    for (idx_t i = 0; i < r.RowCount(); i++) {
        ids.push_back(r.GetValue(0, i).GetBigInt());
    }
    return ids;
}

} // namespace

TEST(ParallelQuery, TheSameQueriesGiveTheSameAnswersOnOneAndFourThreads) {
    const ForceParallelPaths force;
    Database one(1), four(4);
    Connection c1(one), c4(four);
    Populate(c1);
    Populate(c4);
    ASSERT_EQ(c4.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(102400));
    for (const char* sql : kQueries) {
        const QueryResult a = c1.Query(sql), b = c4.Query(sql);
        ASSERT_TRUE(a.ok()) << sql << ": " << a.error_message();
        ASSERT_TRUE(b.ok()) << sql << ": " << b.error_message();
        EXPECT_EQ(b.ToString(), a.ToString()) << sql;
    }
}

// SUM and AVG of a DOUBLE column are the exact sum rounded once, so they do not depend on how many
// threads added what in which order. Values here are not exactly representable (random 53-bit
// fractions over several magnitudes), so a plain parallel sum would differ in the last bits.
TEST(ParallelQuery, FloatingPointSumsAndAveragesAreBitIdenticalOnAnyNumberOfThreads) {
    std::vector<std::vector<std::string>> answers; // per configuration: every cell of every query
    const char* const queries[] = {
        "SELECT sum(x), avg(x), sum(y), avg(y), count(*) FROM f",
        "SELECT g, sum(x), avg(x), sum(x * y) FROM f GROUP BY g ORDER BY g",
        "SELECT id % 997 AS k, sum(x), avg(y) FROM f GROUP BY k ORDER BY k",
        "SELECT g, sum(x) FROM f WHERE y > 0 GROUP BY g HAVING sum(x) > 0 ORDER BY g",
    };
    for (const size_t threads : {size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{8}}) {
        for (const bool one_vector_morsels : {true, false}) {
            Database db(threads);
            Connection conn(db);
            ForceParallelPaths force(one_vector_morsels);
            MustSucceed(conn, "CREATE TABLE f (id BIGINT, g INTEGER, x DOUBLE, y DOUBLE)");
            std::mt19937_64 rng(5);
            std::string values;
            for (int i = 0; i < 30000; i++) {
                const double magnitude = std::ldexp(1.0, static_cast<int>(rng() % 24) - 8);
                const double x =
                    (static_cast<double>(rng() >> 11) / 9007199254740992.0 - 0.5) * magnitude;
                const double y =
                    (static_cast<double>(rng() >> 11) / 9007199254740992.0 - 0.3) * 1000.0;
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "(%d, %d, %.17g, %.17g)", i, i % 7, x, y);
                values += (i ? "," : "") + std::string(buffer);
            }
            MustSucceed(conn, "INSERT INTO f VALUES " + values);
            std::vector<std::string> cells;
            for (const char* sql : queries) {
                const QueryResult r = conn.Query(sql);
                ASSERT_TRUE(r.ok()) << sql << ": " << r.error_message();
                for (idx_t row = 0; row < r.RowCount(); row++) {
                    for (idx_t c = 0; c < r.ColumnCount(); c++) {
                        const Value v = r.GetValue(c, row);
                        if (v.type().id() == TypeId::Double) {
                            uint64_t bits;
                            const double d = v.GetDouble();
                            std::memcpy(&bits, &d, sizeof(bits)); // every bit, not a rounded text
                            cells.push_back(std::to_string(bits));
                        } else {
                            cells.push_back(v.ToString());
                        }
                    }
                }
            }
            answers.push_back(std::move(cells));
        }
    }
    ASSERT_GT(answers.size(), 5U);
    for (size_t i = 1; i < answers.size(); i++) {
        ASSERT_EQ(answers[i].size(), answers[0].size());
        for (size_t c = 0; c < answers[0].size(); c++) {
            ASSERT_EQ(answers[i][c], answers[0][c]) << "configuration " << i << ", cell " << c;
        }
    }
}

// The same aggregate computed twice in one query: the engine inlines a CTE per reference, and the
// two evaluations must agree to the bit (TPC-H Q15: `total_revenue = (SELECT max(total_revenue)
// ...)`).
TEST(ParallelQuery, ARecomputedAggregateEqualsItselfOnEveryThreadCount) {
    for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}}) {
        Database db(threads);
        Connection conn(db);
        const ForceParallelPaths force;
        MustSucceed(conn, "CREATE TABLE f (id BIGINT, g INTEGER, x DOUBLE)");
        std::mt19937_64 rng(11);
        std::string values;
        for (int i = 0; i < 40000; i++) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "(%d, %d, %.17g)", i, i % 5000,
                          (static_cast<double>(rng() >> 11) / 9007199254740992.0) * 1000.0);
            values += (i ? "," : "") + std::string(buffer);
        }
        MustSucceed(conn, "INSERT INTO f VALUES " + values);
        const QueryResult differing =
            conn.Query("WITH a AS (SELECT g, sum(x) AS s FROM f GROUP BY g) "
                       "SELECT count(*) FROM a, (SELECT g, sum(x) AS s FROM f GROUP BY g) b "
                       "WHERE a.g = b.g AND a.s <> b.s");
        ASSERT_TRUE(differing.ok()) << differing.error_message();
        EXPECT_EQ(differing.GetValue(0, 0).GetBigInt(), 0) << threads << " threads";
        const QueryResult top =
            conn.Query("WITH r AS (SELECT g, sum(x) AS total FROM f GROUP BY g) "
                       "SELECT g FROM r WHERE total = (SELECT max(total) FROM r)");
        ASSERT_TRUE(top.ok()) << top.error_message();
        EXPECT_EQ(top.RowCount(), 1U) << threads << " threads: Q15's shape finds its maximum";
    }
}

TEST(ParallelQuery, ASmallTableIsSplitAcrossThreadsAndGivesTheSameAnswers) {
    // The built-in morsel size, not the forced one: a 12,800-row table is a single 16,384-row
    // morsel unless the scan sizes its morsels by the thread count.
    const ForceParallelPaths force(false);
    Database one(1), eight(8);
    Connection c1(one), c8(eight);
    Populate(c1, 7);
    Populate(c8, 7);
    ASSERT_EQ(c8.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(12800));
    for (const char* sql : kQueries) {
        const QueryResult a = c1.Query(sql), b = c8.Query(sql);
        ASSERT_TRUE(a.ok()) << sql << ": " << a.error_message();
        ASSERT_TRUE(b.ok()) << sql << ": " << b.error_message();
        EXPECT_EQ(b.ToString(), a.ToString()) << sql;
    }
    const std::vector<int64_t> ids = Ids(c8, "SELECT id FROM t WHERE a > 5 AND g < 50");
    ASSERT_GT(ids.size(), 1000U);
    for (size_t i = 1; i < ids.size(); i++) {
        ASSERT_LT(ids[i - 1], ids[i]) << "table order across the adaptive morsels, row " << i;
    }
}

TEST(ParallelQuery, ScanFilterAndInsertKeepTableOrderOnSeveralThreads) {
    const ForceParallelPaths force;
    Database four(4);
    Connection conn(four);
    Populate(conn);
    // no ORDER BY: a plain scan -> filter -> collector pipeline returns rows in table order
    for (int round = 0; round < 3; round++) {
        const std::vector<int64_t> ids = Ids(conn, "SELECT id FROM t WHERE a > 5 AND g < 50");
        ASSERT_GT(ids.size(), 10000U);
        for (size_t i = 1; i < ids.size(); i++) {
            ASSERT_LT(ids[i - 1], ids[i]) << "row " << i << " round " << round;
        }
    }
    // INSERT ... SELECT stages rows in the order a single thread would
    MustSucceed(conn, "CREATE TABLE t2 (id BIGINT, a INTEGER)");
    MustSucceed(conn, "INSERT INTO t2 SELECT id, a FROM t WHERE a > 3");
    const std::vector<int64_t> stored = Ids(conn, "SELECT id FROM t2");
    ASSERT_GT(stored.size(), 10000U);
    for (size_t i = 1; i < stored.size(); i++) {
        ASSERT_LT(stored[i - 1], stored[i]) << "t2 row " << i;
    }
    EXPECT_EQ(stored, Ids(conn, "SELECT id FROM t WHERE a > 3"));
}

TEST(ParallelQuery, ALimitWithoutOrderReturnsTheFirstRowsOfTheTable) {
    const ForceParallelPaths force;
    Database four(4);
    Connection conn(four);
    Populate(conn);
    const std::vector<int64_t> ids = Ids(conn, "SELECT id FROM t LIMIT 5000 OFFSET 123");
    ASSERT_EQ(ids.size(), 5000U);
    for (size_t i = 0; i < ids.size(); i++) {
        ASSERT_EQ(ids[i], static_cast<int64_t>(123 + i))
            << "LIMIT runs on one thread, in table order";
    }
}

TEST(ParallelQuery, SeveralConnectionsShareOnePool) {
    const ForceParallelPaths force;
    Database db(4);
    {
        Connection setup(db);
        Populate(setup);
    }
    Database reference(1);
    std::vector<std::string> expected;
    {
        Connection c(reference);
        Populate(c);
        for (const char* sql : kQueries) {
            expected.push_back(c.Query(sql).ToString());
        }
    }
    std::vector<std::thread> sessions;
    std::atomic<int> mismatches{0};
    for (int t = 0; t < 4; t++) {
        sessions.emplace_back([&, t] {
            Connection conn(db);
            for (int round = 0; round < 2; round++) {
                for (size_t q = 0; q < std::size(kQueries); q++) {
                    const size_t pick = (q + static_cast<size_t>(t) * 3) % std::size(kQueries);
                    if (conn.Query(kQueries[pick]).ToString() != expected[pick]) {
                        mismatches++;
                    }
                }
            }
        });
    }
    for (std::thread& s : sessions) {
        s.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(ParallelQuery, AnErrorOnOneThreadFailsTheStatementAndTheDatabaseKeepsWorking) {
    const ForceParallelPaths force;
    Database four(4);
    Connection conn(four);
    Populate(conn);
    // SUM of every id times a big constant overflows BIGINT: raised when the result is produced
    const QueryResult bad = conn.Query("SELECT sum(id * 1000000000000) FROM t");
    ASSERT_FALSE(bad.ok());
    EXPECT_NE(bad.error_message().find("overflow"), std::string::npos) << bad.error_message();
    // an integer overflow in an expression, on some thread
    const QueryResult bad2 = conn.Query("SELECT count(*) FROM t WHERE a + 2147483647 > 0");
    ASSERT_FALSE(bad2.ok());
    ASSERT_TRUE(conn.Query("SELECT count(*) FROM t").ok());
    EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(102400));
}

TEST(Database, ThreadsAreConfigurableAndRunningQueriesKeepTheirPool) {
    Database db(4);
    EXPECT_EQ(db.threads(), 4U);
    const std::shared_ptr<TaskScheduler> pool = db.scheduler();
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->threads(), 4U);

    db.SetThreads(1);
    EXPECT_EQ(db.threads(), 1U);
    EXPECT_EQ(db.scheduler(), nullptr) << "one thread means no pool: queries run on the caller";
    EXPECT_EQ(pool->threads(), 4U) << "a query that started earlier still has its pool";
    std::atomic<int> n{0};
    pool->ParallelFor(10, [&](size_t) { n++; });
    EXPECT_EQ(n.load(), 10);

    db.SetThreads(0);
    EXPECT_EQ(db.threads(), TaskScheduler::HardwareThreads()) << "0 means one per hardware thread";
    db.SetThreads(3);
    ASSERT_NE(db.scheduler(), nullptr);
    EXPECT_EQ(db.scheduler()->threads(), 3U);

    EXPECT_EQ(Database(1).scheduler(), nullptr);
    EXPECT_EQ(Database(2).scheduler()->threads(), 2U);
}

TEST(Database, DefaultThreadsComeFromTheEnvironmentAndFallBackToOne) {
    const char* old = std::getenv("CDB_THREADS");
    const std::string saved = old != nullptr ? old : "";
    const auto restore = [&] {
        if (old != nullptr) {
            setenv("CDB_THREADS", saved.c_str(), 1);
        } else {
            unsetenv("CDB_THREADS");
        }
    };
    unsetenv("CDB_THREADS");
    EXPECT_EQ(Database::DefaultThreads(), 1U);
    setenv("CDB_THREADS", "3", 1);
    EXPECT_EQ(Database::DefaultThreads(), 3U);
    EXPECT_EQ(Database().threads(), 3U);
    setenv("CDB_THREADS", "0", 1);
    EXPECT_EQ(Database::DefaultThreads(), TaskScheduler::HardwareThreads());
    setenv("CDB_THREADS", "auto", 1);
    EXPECT_EQ(Database::DefaultThreads(), TaskScheduler::HardwareThreads());
    setenv("CDB_THREADS", "banana", 1);
    EXPECT_EQ(Database::DefaultThreads(), 1U) << "not a number: stay serial";
    setenv("CDB_THREADS", "-2", 1);
    EXPECT_EQ(Database::DefaultThreads(), 1U);
    setenv("CDB_THREADS", "", 1);
    EXPECT_EQ(Database::DefaultThreads(), 1U);
    restore();
}

} // namespace cdb
