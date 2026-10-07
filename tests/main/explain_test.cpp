// EXPLAIN (estimated rows per operator) and EXPLAIN ANALYZE (what each operator really did).

#include "main/connection.h"
#include "main/explain.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <sstream>

namespace cdb {

namespace {

struct Session {
    Database db;
    Connection conn{db};
    explicit Session(size_t threads = 1) : db(threads) {}

    void Run(const std::string& sql) {
        const QueryResult r = conn.Query(sql);
        ASSERT_TRUE(r.ok()) << sql.substr(0, 150) << "\n" << r.error_message();
    }
    std::vector<std::string> Lines(const std::string& sql) {
        const QueryResult r = conn.Query(sql);
        EXPECT_TRUE(r.ok()) << sql << "\n" << r.error_message();
        std::vector<std::string> out;
        for (idx_t i = 0; i < r.RowCount(); i++) {
            out.push_back(r.GetValue(0, i).GetVarchar());
        }
        return out;
    }
    int64_t Count(const std::string& sql) {
        const QueryResult r = conn.Query("SELECT count(*) FROM (" + sql + ") AS q");
        EXPECT_TRUE(r.ok()) << r.error_message();
        return r.GetValue(0, 0).GetBigInt();
    }
};

// One operator line of EXPLAIN ANALYZE.
struct Line {
    std::string text; // the operator, without the indentation and the statistics
    int depth = 0;
    int64_t estimate = -1;
    int64_t actual = -1;
    double ms = -1;
    int64_t build_rows = -1; // a join's build side
    int64_t in_rows = -1;    // what a sink consumed
    bool merged = false;
};

// One line of EXPLAIN ANALYZE, parsed by hand (std::regex trips GCC 13's -Wmaybe-uninitialized
// under -O2 -Werror): `<indent><operator>  (est ~E, actual A rows, T ms[; build B rows, T ms | ; in
// N rows])` or `<indent><operator>  (sorted as a top-N together with the LIMIT above)`.
bool ParseLine(const std::string& l, Line& out) {
    size_t indent = 0;
    while (indent < l.size() && l[indent] == ' ') {
        indent++;
    }
    const size_t merged = l.find("  (sorted as a top-N together with the LIMIT above)");
    const size_t stats = l.find("  (est ~");
    if (merged != std::string::npos) {
        out.depth = static_cast<int>(indent / 2);
        out.text = l.substr(indent, merged - indent);
        out.merged = true;
        return true;
    }
    if (stats == std::string::npos) {
        return false;
    }
    long long estimate = 0, actual = 0, build = 0, consumed = 0;
    double ms = 0;
    int used = 0;
    const char* p = l.c_str() + stats + 2;
    if (std::sscanf(p, "(est ~%lld, actual %lld rows, %lf ms%n", &estimate, &actual, &ms, &used) !=
        3) {
        return false;
    }
    out.depth = static_cast<int>(indent / 2);
    out.text = l.substr(indent, stats - indent);
    out.estimate = estimate;
    out.actual = actual;
    out.ms = ms;
    const char* rest = p + used;
    if (std::sscanf(rest, "; build %lld rows", &build) == 1) {
        out.build_rows = build;
    } else if (std::sscanf(rest, "; in %lld rows", &consumed) == 1) {
        out.in_rows = consumed;
    }
    return true;
}

std::vector<Line> ParseAnalyze(const std::vector<std::string>& lines) {
    std::vector<Line> out;
    for (const std::string& l : lines) {
        Line line;
        if (ParseLine(l, line)) { // (the summary lines are not operators)
            out.push_back(line);
        }
    }
    return out;
}

const Line* Find(const std::vector<Line>& lines, const std::string& prefix) {
    for (const Line& l : lines) {
        if (l.text.rfind(prefix, 0) == 0) {
            return &l;
        }
    }
    return nullptr;
}

void LoadData(Session& s) {
    s.Run("CREATE TABLE fact (id INTEGER, k INTEGER, v INTEGER, tag VARCHAR)");
    s.Run("CREATE TABLE dim (k INTEGER, name VARCHAR)");
    std::string fact, dim;
    for (int i = 0; i < 20000; i++) {
        fact += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                std::to_string(i % 200) + "," + std::to_string(i % 7) + ",'" +
                (i % 4 == 0 ? "a" : "b") + "')";
    }
    for (int i = 0; i < 100; i++) {
        dim += (i ? "," : "") + std::string("(") + std::to_string(i) + ",'d" + std::to_string(i) +
               "')";
    }
    s.Run("INSERT INTO fact VALUES " + fact);
    s.Run("INSERT INTO dim VALUES " + dim);
}

} // namespace

TEST(Explain, EveryOperatorShowsItsEstimateAndAScanIsExact) {
    Session s;
    LoadData(s);
    const auto lines = s.Lines("EXPLAIN SELECT tag, count(*) FROM fact JOIN dim ON fact.k = dim.k "
                               "WHERE v < 3 GROUP BY tag");
    for (const std::string& l : lines) {
        std::string shape;
        EXPECT_TRUE(test::StripEstimate(l, &shape)) << l;
        EXPECT_FALSE(shape.empty()) << l;
        EXPECT_NE(shape.find_first_not_of(' '), std::string::npos) << l;
        EXPECT_TRUE(std::isupper(static_cast<unsigned char>(shape[shape.find_first_not_of(' ')])))
            << l;
    }
    const auto scan = s.Lines("EXPLAIN SELECT * FROM fact");
    ASSERT_EQ(scan.size(), 2U) << "a PROJECT over the scan";
    EXPECT_NE(scan[1].find("SCAN fact"), std::string::npos);
    EXPECT_NE(scan[1].find("(~20000 rows)"), std::string::npos) << scan[1];
    // a filter on a value that is in every segment's bounds but selects a seventh of the rows
    const auto filtered = s.Lines("EXPLAIN SELECT * FROM fact WHERE v = 3");
    bool found = false;
    for (const std::string& l : filtered) {
        if (l.find("FILTER") != std::string::npos) {
            found = true;
            const int64_t rows = std::stoll(l.substr(l.rfind("~") + 1));
            EXPECT_NEAR(static_cast<double>(rows), 20000.0 / 7, 20000.0 / 7 * 0.05) << l;
        }
    }
    EXPECT_TRUE(found);
    // an empty table estimates no rows
    s.Run("CREATE TABLE empty_t (a INTEGER)");
    EXPECT_NE(s.Lines("EXPLAIN SELECT * FROM empty_t")[1].find("(~0 rows)"), std::string::npos);
}

TEST(ExplainAnalyze, ActualRowsOfEveryOperatorAreTheTrueCounts) {
    Session s;
    LoadData(s);
    const auto lines =
        s.Lines("EXPLAIN ANALYZE SELECT tag, count(*) AS n FROM fact JOIN dim ON fact.k = dim.k "
                "WHERE v < 3 GROUP BY tag ORDER BY n DESC LIMIT 5");
    const std::vector<Line> plan = ParseAnalyze(lines);
    ASSERT_GE(plan.size(), 6U);
    // the root is what the query returns
    EXPECT_EQ(plan[0].actual, 2) << "tags a and b";
    EXPECT_EQ(plan[0].depth, 0);

    const Line* scan_fact = Find(plan, "SCAN fact");
    const Line* scan_dim = Find(plan, "SCAN dim");
    const Line* join = Find(plan, "JOIN INNER");
    const Line* filter = Find(plan, "FILTER");
    const Line* aggregate = Find(plan, "AGGREGATE");
    ASSERT_TRUE(scan_fact && scan_dim && join && filter && aggregate);
    EXPECT_EQ(scan_fact->actual, 20000);
    EXPECT_EQ(scan_dim->actual, 100);
    EXPECT_EQ(filter->actual, s.Count("SELECT * FROM fact WHERE v < 3"));
    EXPECT_EQ(join->actual, s.Count("SELECT * FROM fact JOIN dim ON fact.k = dim.k WHERE v < 3"));
    EXPECT_EQ(join->build_rows, 100) << "the smaller input is built";
    EXPECT_EQ(aggregate->actual, 2);
    EXPECT_EQ(aggregate->in_rows, join->actual) << "it aggregates what the join produced";
    // the ORDER BY under the LIMIT is a top-N together with it
    const Line* limit = Find(plan, "LIMIT");
    ASSERT_NE(limit, nullptr);
    EXPECT_EQ(limit->actual, 2);
    EXPECT_EQ(limit->in_rows, 2);
    const Line* order = Find(plan, "ORDER BY");
    ASSERT_NE(order, nullptr);
    EXPECT_TRUE(order->merged);
    // times are non-negative numbers and the summary says what ran
    for (const Line& l : plan) {
        if (!l.merged) {
            EXPECT_GE(l.ms, 0.0) << l.text;
        }
    }
    std::string text;
    for (const std::string& l : lines) {
        text += l + "\n";
    }
    EXPECT_NE(text.find("Planning: "), std::string::npos);
    EXPECT_NE(text.find("Execution: "), std::string::npos);
    EXPECT_NE(text.find("1 thread, 2 rows returned"), std::string::npos) << text;
}

TEST(ExplainAnalyze, ZoneMapPruningShowsInTheRowsAScanProduced) {
    Session s;
    // 3 row groups of ascending ids: `id < 1000` can only be in the first
    s.Run("CREATE TABLE t (id INTEGER, x INTEGER)");
    std::string values;
    for (int i = 0; i < 3 * 122880 / 8; i++) {
        values += (i ? "," : "") + std::string("(") + std::to_string(i) + ",1)";
    }
    s.Run("INSERT INTO t VALUES " + values);
    s.Run("INSERT INTO t SELECT id + 50000, x FROM t");
    s.Run("INSERT INTO t SELECT id + 100000, x FROM t");
    s.Run("INSERT INTO t SELECT id + 200000, x FROM t");
    s.Run("INSERT INTO t SELECT id + 400000, x FROM t");
    const int64_t total = s.Count("SELECT * FROM t");
    ASSERT_GT(total, 122880) << "more than one row group";
    const std::vector<Line> plan =
        ParseAnalyze(s.Lines("EXPLAIN ANALYZE SELECT * FROM t WHERE id < 1000"));
    const Line* scan = Find(plan, "SCAN t");
    const Line* filter = Find(plan, "FILTER");
    ASSERT_TRUE(scan && filter);
    EXPECT_LT(scan->actual, total) << "row groups that cannot match are not read";
    EXPECT_EQ(filter->actual, 1000);
    EXPECT_NEAR(static_cast<double>(filter->estimate), 1000.0, 1000.0 * 0.3);
}

TEST(ExplainAnalyze, SubqueriesAndEveryJoinTypeAreReported) {
    Session s;
    LoadData(s);
    for (const std::string q :
         {"SELECT id FROM fact WHERE EXISTS (SELECT 1 FROM dim WHERE dim.k = fact.k)",
          "SELECT id FROM fact WHERE NOT EXISTS (SELECT 1 FROM dim WHERE dim.k = fact.k)",
          "SELECT id FROM fact WHERE k IN (SELECT k FROM dim WHERE k < 10)",
          "SELECT id FROM fact WHERE k NOT IN (SELECT k FROM dim WHERE k < 10)",
          "SELECT id FROM fact WHERE v > (SELECT avg(v) FROM fact)",
          "SELECT id, (SELECT count(*) FROM dim WHERE dim.k = fact.k) FROM fact WHERE id < 500",
          "SELECT fact.id, dim.name FROM fact LEFT JOIN dim ON fact.k = dim.k WHERE fact.id < 1000",
          "WITH x AS (SELECT k FROM dim WHERE k < 20) SELECT count(*) FROM fact JOIN x ON fact.k = "
          "x.k",
          "SELECT DISTINCT tag, v FROM fact",
          "SELECT * FROM fact ORDER BY v DESC, id LIMIT 10 OFFSET 5", "SELECT 1"}) {
        const std::vector<Line> plan = ParseAnalyze(s.Lines("EXPLAIN ANALYZE " + q));
        ASSERT_FALSE(plan.empty()) << q;
        EXPECT_EQ(plan[0].actual, s.Count(q)) << q;
        for (const Line& l : plan) {
            if (!l.merged) {
                EXPECT_GE(l.actual, 0) << q << ": " << l.text;
            }
        }
    }
}

TEST(ExplainAnalyze, TheRowCountsDoNotDependOnTheNumberOfThreads) {
    const std::string query = "SELECT tag, count(*), sum(v) FROM fact JOIN dim ON fact.k = dim.k "
                              "WHERE v < 6 GROUP BY tag";
    std::vector<std::vector<std::pair<std::string, int64_t>>> runs;
    for (const size_t threads : {size_t{1}, size_t{4}}) {
        Session s(threads);
        LoadData(s);
        const auto lines = s.Lines("EXPLAIN ANALYZE " + query);
        std::vector<std::pair<std::string, int64_t>> counts;
        for (const Line& l : ParseAnalyze(lines)) {
            counts.emplace_back(l.text, l.actual);
        }
        runs.push_back(counts);
        std::string all;
        for (const auto& l : lines) {
            all += l + "\n";
        }
        EXPECT_NE(all.find(std::to_string(threads) + (threads == 1 ? " thread" : " threads")),
                  std::string::npos)
            << all;
    }
    EXPECT_EQ(runs[0], runs[1]);
}

TEST(ExplainAnalyze, TimesAreCpuTimesSummedOverThreadsAndNeverExceedThem) {
    Session s(4);
    LoadData(s);
    const auto lines = s.Lines(
        "EXPLAIN ANALYZE SELECT tag, sum(v) FROM fact JOIN dim ON fact.k = dim.k GROUP BY tag");
    double execution_ms = -1;
    for (const std::string& l : lines) {
        double ms = 0;
        int threads = 0;
        if (std::sscanf(l.c_str(), "Execution: %lf ms on %d thread", &ms, &threads) == 2) {
            execution_ms = ms;
            EXPECT_EQ(threads, 4);
        }
    }
    ASSERT_GE(execution_ms, 0);
    for (const Line& l : ParseAnalyze(lines)) {
        // an operator's time is at most the time of all four threads for the whole run
        EXPECT_LE(l.ms, 4 * execution_ms + 0.05) << l.text;
    }
}

TEST(ExplainAnalyze, WorksWithTheOptimizerOffAndFailsCleanlyForWhatIsNotAQuery) {
    Session s;
    LoadData(s);
    s.conn.SetOptimizerEnabled(false);
    const std::vector<Line> plan =
        ParseAnalyze(s.Lines("EXPLAIN ANALYZE SELECT * FROM fact, dim WHERE fact.k = dim.k"));
    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(plan[0].actual, s.Count("SELECT * FROM fact, dim WHERE fact.k = dim.k"));
    s.conn.SetOptimizerEnabled(true);
    for (const std::string q : {"EXPLAIN ANALYZE INSERT INTO dim VALUES (5, 'x')",
                                "EXPLAIN ANALYZE CREATE TABLE z (a INT)",
                                "EXPLAIN ANALYZE DROP TABLE dim", "EXPLAIN ANALYZE CHECKPOINT"}) {
        const QueryResult r = s.conn.Query(q);
        EXPECT_FALSE(r.ok()) << q;
        EXPECT_EQ(r.error_code(), ErrorCode::NotImplemented) << q;
    }
    EXPECT_EQ(s.Count("SELECT * FROM dim"), 100) << "nothing ran";
    EXPECT_EQ(s.conn.Query("SELECT * FROM z").ok(), false);
    // a query that fails while running reports its error
    const QueryResult failing =
        s.conn.Query("EXPLAIN ANALYZE SELECT (SELECT k FROM dim) FROM fact");
    EXPECT_FALSE(failing.ok());
    EXPECT_EQ(failing.error_code(), ErrorCode::Execution);
}

TEST(ExplainAnalyze, ARandomizedCheckThatTheRootAlwaysReportsTheResultSize) {
    test::Rng rng(21);
    Session s;
    s.Run("CREATE TABLE r1 (a INTEGER, b VARCHAR, c DOUBLE)");
    s.Run("CREATE TABLE r2 (a INTEGER, d INTEGER, e VARCHAR)");
    std::string v1, v2;
    for (int i = 0; i < 300; i++) {
        v1 += (i ? "," : "") + std::string("(") + std::to_string(test::RandBelow(rng, 20)) + ",'k" +
              std::to_string(test::RandBelow(rng, 7)) + "'," +
              std::to_string(test::RandBelow(rng, 50)) + ".5)";
        v2 += (i ? "," : "") + std::string("(") + std::to_string(test::RandBelow(rng, 25)) + "," +
              std::to_string(test::RandBelow(rng, 10)) + ",'z" +
              std::to_string(test::RandBelow(rng, 3)) + "')";
    }
    s.Run("INSERT INTO r1 VALUES " + v1);
    s.Run("INSERT INTO r2 VALUES " + v2);
    const std::vector<std::string> shapes = {
        "SELECT * FROM r1 WHERE a < %d",
        "SELECT r1.a, r2.d FROM r1 JOIN r2 ON r1.a = r2.a WHERE r2.d < %d",
        "SELECT b, count(*) FROM r1 WHERE a > %d GROUP BY b",
        "SELECT a FROM r1 WHERE a IN (SELECT a FROM r2 WHERE d < %d)",
        "SELECT a FROM r1 WHERE NOT EXISTS (SELECT 1 FROM r2 WHERE r2.a = r1.a AND r2.d > %d)",
        "SELECT r1.b, r2.e FROM r1 LEFT JOIN r2 ON r1.a = r2.a AND r2.d = %d",
        "SELECT DISTINCT d FROM r2 WHERE a > %d",
        "SELECT * FROM r1 ORDER BY c DESC LIMIT %d",
    };
    for (int round = 0; round < 80; round++) {
        char sql[256];
        std::snprintf(sql, sizeof(sql), shapes[static_cast<size_t>(round) % shapes.size()].c_str(),
                      static_cast<int>(test::RandBelow(rng, 25)));
        const std::vector<Line> plan = ParseAnalyze(s.Lines(std::string("EXPLAIN ANALYZE ") + sql));
        ASSERT_FALSE(plan.empty()) << sql;
        EXPECT_EQ(plan[0].actual, s.Count(sql)) << sql;
        // a child never reports more rows than a join / cross product could, and a filter never
        // grows its input
        for (size_t i = 0; i < plan.size(); i++) {
            if (plan[i].text.rfind("FILTER", 0) == 0 && i + 1 < plan.size() &&
                plan[i + 1].depth == plan[i].depth + 1 && !plan[i + 1].merged) {
                EXPECT_LE(plan[i].actual, plan[i + 1].actual) << sql;
            }
        }
    }
}

} // namespace cdb
