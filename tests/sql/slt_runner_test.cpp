// Runs the sqllogictest-style files in tests/sql/*.test against this engine. The expected results
// in them were produced by DuckDB (tools/gen_slt.py), so these are differential tests that do not
// need DuckDB at run time. See tools/gen_slt.py for the format.

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cdb {

namespace {

namespace fs = std::filesystem;

struct Block {
    enum class Kind { StatementOk, StatementError, Query } kind;
    int line = 0; // 1-based line of the header
    std::string header;
    std::string sql;
    bool rowsort = false;
    size_t columns = 0;
    std::vector<std::string> expected;
};

std::vector<Block> ParseFile(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        lines.push_back(l);
    }
    std::vector<Block> blocks;
    size_t i = 0;
    while (i < lines.size()) {
        const std::string& line = lines[i];
        const bool is_statement = line.rfind("statement ", 0) == 0;
        const bool is_query = line.rfind("query ", 0) == 0;
        if (!is_statement && !is_query) {
            i++;
            continue;
        }
        Block b;
        b.line = static_cast<int>(i) + 1;
        b.header = line;
        std::istringstream words(line);
        std::string kind, arg, flag;
        words >> kind >> arg;
        if (is_statement) {
            b.kind = arg == "ok" ? Block::Kind::StatementOk : Block::Kind::StatementError;
        } else {
            b.kind = Block::Kind::Query;
            b.columns = arg.size();
            while (words >> flag) {
                b.rowsort = b.rowsort || flag == "rowsort";
            }
        }
        i++;
        std::string sql;
        while (i < lines.size() && !lines[i].empty() && lines[i] != "----") {
            sql += (sql.empty() ? "" : "\n") + lines[i++];
        }
        b.sql = sql;
        if (i < lines.size() && lines[i] == "----") {
            i++;
            while (i < lines.size() && !lines[i].empty()) {
                b.expected.push_back(lines[i++]);
            }
        }
        blocks.push_back(std::move(b));
    }
    return blocks;
}

// Must match render() in tools/gen_slt.py.
std::string Render(const Value& v) {
    if (v.IsNull()) {
        return "NULL";
    }
    if (v.type().id() == TypeId::Varchar && v.GetVarchar().empty()) {
        return "(empty)";
    }
    return v.ToString();
}

// ---- comparing a result with DuckDB's, to the last digit or to rounding
// ---------------------------

std::vector<std::string> SplitCells(const std::string& line) {
    std::vector<std::string> cells;
    size_t from = 0;
    for (;;) {
        const size_t tab = line.find('\t', from);
        cells.push_back(line.substr(from, tab == std::string::npos ? tab : tab - from));
        if (tab == std::string::npos) {
            return cells;
        }
        from = tab + 1;
    }
}

bool AsNumber(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return !s.empty() && end == s.c_str() + s.size();
}

// Two cells are the same if their text is, or both are numbers equal to 1e-12 relative: DuckDB
// averages integers and adds doubles in an order and with a rounding of its own, so the last digit
// of such an answer is not part of the contract (ADR 0003).
bool CellsMatch(const std::string& a, const std::string& b) {
    if (a == b) {
        return true;
    }
    double x, y;
    if (!AsNumber(a, x) || !AsNumber(b, y)) {
        return false;
    }
    return (std::isnan(x) && std::isnan(y)) ||
           std::fabs(x - y) <= 1e-12 * std::max(std::fabs(x), std::fabs(y));
}

bool LinesMatch(const std::string& a, const std::string& b) {
    const auto x = SplitCells(a), y = SplitCells(b);
    if (x.size() != y.size()) {
        return false;
    }
    for (size_t i = 0; i < x.size(); i++) {
        if (!CellsMatch(x[i], y[i])) {
            return false;
        }
    }
    return true;
}

// A sort key that does not change when a number moves in its last digits.
std::string RoundedKey(const std::string& line) {
    std::string key;
    for (const std::string& cell : SplitCells(line)) {
        double v;
        if (AsNumber(cell, v) && cell.find_first_of(".eE") != std::string::npos) {
            char buffer[40];
            std::snprintf(buffer, sizeof(buffer), "%.9g", v);
            key += buffer;
        } else {
            key += cell;
        }
        key += '\t';
    }
    return key;
}

bool ResultsMatch(std::vector<std::string> got, std::vector<std::string> expected, bool rowsort,
                  bool approximate) {
    if (!approximate) {
        return got == expected;
    }
    if (got.size() != expected.size()) {
        return false;
    }
    if (rowsort) {
        const auto by_key = [](const std::string& a, const std::string& b) {
            return RoundedKey(a) < RoundedKey(b);
        };
        std::stable_sort(got.begin(), got.end(), by_key);
        std::stable_sort(expected.begin(), expected.end(), by_key);
    }
    for (size_t i = 0; i < got.size(); i++) {
        if (!LinesMatch(got[i], expected[i])) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> SqlFiles() {
    std::vector<std::string> files;
    const fs::path dir = fs::path(CDB_SOURCE_DIR) / "tests" / "sql";
    if (fs::exists(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".test") {
                files.push_back(e.path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

// The database a file runs against: in memory, or a persistent one that is killed and recovered
// after every single statement (the machine loses power, everything not fsynced is gone, and the
// next statement runs on what recovery makes of the disk). The expected results are the same.
class Subject {
  public:
    explicit Subject(bool persistent) : persistent_(persistent) {
        if (persistent_) {
            disk_ = std::make_shared<MemoryFileSystem>();
            Open();
        } else {
            DatabaseOptions o;
            o.row_group_size = 2 * kVectorSize; // several row groups even in small tables
            db_ = std::make_unique<Database>(o);
        }
        conn_ = std::make_unique<Connection>(*db_);
        conn_->SetOptimizerEnabled(optimizer_);
    }
    QueryResult Query(const std::string& sql) {
        QueryResult r = conn_->Query(sql);
        if (persistent_) {
            conn_.reset();
            const auto survivors = disk_->Crash(CrashPolicy::DropUnsynced());
            db_.reset(); // closing writes into the old disk, which is gone
            disk_ = survivors;
            Open();
            conn_ = std::make_unique<Connection>(*db_);
            conn_->SetOptimizerEnabled(optimizer_);
        }
        return r;
    }
    int restarts() const { return restarts_; }
    void SetOptimizer(bool enabled) {
        optimizer_ = enabled;
        conn_->SetOptimizerEnabled(enabled);
    }

  private:
    void Open() {
        DatabaseOptions o;
        o.storage.fs = disk_;
        o.storage.checkpoint_wal_bytes = 64 * 1024; // recoveries from checkpoints and from the log
        db_ = std::make_unique<Database>("/db", o);
        restarts_++;
    }
    bool persistent_;
    std::shared_ptr<MemoryFileSystem> disk_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Connection> conn_;
    int restarts_ = 0;
    bool optimizer_ = true;
};

// How a file is run. A generated file may contain queries that need something this engine does not
// have (it answers NotImplemented): those are counted instead of failing, and `unsupported` is
// where the count goes.
struct RunOptions {
    bool persistent = false;
    bool optimizer = true;
    int* unsupported = nullptr;
    // Doubles may differ from DuckDB's in the last digits (generated files, which average and add
    // freely; the hand-written files keep every digit)
    bool approximate_doubles = false;
};

void RunSqlFile(const std::string& path, const RunOptions& options) {
    const std::vector<Block> blocks = ParseFile(path);
    ASSERT_FALSE(blocks.empty()) << path;
    const bool persistent = options.persistent;
    Subject subject(persistent);
    subject.SetOptimizer(options.optimizer);
    int queries = 0;
    for (const Block& b : blocks) {
        const std::string where = fs::path(path).filename().string() + ":" + std::to_string(b.line);
        if (std::getenv("CDB_SLT_VERBOSE") != nullptr) { // find the statement a run is stuck on
            std::cerr << where << ": " << b.sql.substr(0, 400) << std::endl;
        }
        const QueryResult r = subject.Query(b.sql);
        switch (b.kind) {
        case Block::Kind::StatementOk:
            ASSERT_TRUE(r.ok()) << where << "\n" << b.sql << "\n" << r.error_message();
            break;
        case Block::Kind::StatementError:
            ASSERT_FALSE(r.ok()) << where << ": expected an error\n" << b.sql;
            break;
        case Block::Kind::Query: {
            queries++;
            if (!r.ok() && options.unsupported != nullptr &&
                r.error_code() == ErrorCode::NotImplemented) {
                ++*options.unsupported;
                break;
            }
            ASSERT_TRUE(r.ok()) << where << "\n" << b.sql << "\n" << r.error_message();
            ASSERT_EQ(r.ColumnCount(), b.columns) << where << ": column count\n" << b.sql;
            std::vector<std::string> got;
            for (idx_t row = 0; row < r.RowCount(); row++) {
                std::string line;
                for (idx_t c = 0; c < r.ColumnCount(); c++) {
                    line += (c ? "\t" : "") + Render(r.GetValue(c, row));
                }
                got.push_back(std::move(line));
            }
            if (b.rowsort) {
                std::sort(got.begin(), got.end());
            }
            if (!ResultsMatch(got, b.expected, b.rowsort, options.approximate_doubles)) {
                std::string diff;
                const size_t n = std::max(got.size(), b.expected.size());
                int shown = 0;
                for (size_t k = 0; k < n && shown < 8; k++) {
                    const std::string g = k < got.size() ? got[k] : "<missing>";
                    const std::string e = k < b.expected.size() ? b.expected[k] : "<missing>";
                    if (g != e) {
                        diff += "  row " + std::to_string(k) + "\n    got:      " + g +
                                "\n    expected: " + e + "\n";
                        shown++;
                    }
                }
                FAIL() << where << ": result differs from DuckDB (" << got.size() << " rows vs "
                       << b.expected.size() << ")\n"
                       << b.sql << "\n"
                       << diff;
            }
            break;
        }
        }
    }
    EXPECT_GT(queries, 0) << "a .test file with no queries tests nothing";
    if (persistent) {
        EXPECT_GT(subject.restarts(), static_cast<int>(blocks.size()))
            << "the database was recovered after every statement";
    }
}

class SqlLogicTest : public ::testing::TestWithParam<std::string> {};
class SqlLogicPersistentTest : public ::testing::TestWithParam<std::string> {};

TEST_P(SqlLogicTest, MatchesDuckDBExpectedResults) {
    RunSqlFile(GetParam(), {});
}

TEST_P(SqlLogicTest, TheOptimizerDoesNotChangeTheResults) {
    RunSqlFile(GetParam(), {.optimizer = false});
}

TEST_P(SqlLogicPersistentTest, MatchesDuckDBExpectedResultsAcrossACrashAfterEveryStatement) {
    RunSqlFile(GetParam(), {.persistent = true});
}

INSTANTIATE_TEST_SUITE_P(Files, SqlLogicTest, ::testing::ValuesIn(SqlFiles()),
                         [](const ::testing::TestParamInfo<std::string>& param_info) {
                             std::string name = fs::path(param_info.param).stem().string();
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
INSTANTIATE_TEST_SUITE_P(Files, SqlLogicPersistentTest, ::testing::ValuesIn(SqlFiles()),
                         [](const ::testing::TestParamInfo<std::string>& param_info) {
                             std::string name = fs::path(param_info.param).stem().string();
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

// ---------------------------------------------------------------------------------- generated

// Random queries with DuckDB's answers, from tools/fuzz_sql.py (data/sqlfuzz/*.test, not committed:
// generated by the gate, or by hand). Every file runs with the optimizer on and off; both must
// match DuckDB. A query this engine reports as NotImplemented is counted, and there must be few.
TEST(SqlFuzz, RandomQueriesMatchDuckDBWithAndWithoutTheOptimizer) {
    std::vector<std::string> files;
    // (CDB_SQLFUZZ_DIR: where a manual campaign wrote its files, so the gate's stay untouched)
    const char* custom = std::getenv("CDB_SQLFUZZ_DIR");
    const fs::path dir =
        custom != nullptr ? fs::path(custom) : fs::path(CDB_SOURCE_DIR) / "data" / "sqlfuzz";
    if (fs::exists(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".test") {
                files.push_back(e.path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        if (std::getenv("CDB_REQUIRE_SQLFUZZ") != nullptr) {
            FAIL() << "no data/sqlfuzz/*.test: run .venv/bin/python tools/fuzz_sql.py";
        }
        GTEST_SKIP() << "no generated files (tools/fuzz_sql.py writes data/sqlfuzz/*.test)";
    }
    int total_queries = 0, unsupported = 0;
    for (const std::string& file : files) {
        SCOPED_TRACE(file);
        for (const bool optimizer : {true, false}) {
            int skipped = 0;
            RunSqlFile(
                file,
                {.optimizer = optimizer, .unsupported = &skipped, .approximate_doubles = true});
            if (::testing::Test::HasFatalFailure()) {
                return;
            }
            unsupported += skipped;
            if (optimizer) {
                int count = 0;
                for (const Block& b : ParseFile(file)) {
                    count += b.kind == Block::Kind::Query ? 1 : 0;
                }
                total_queries += count;
            }
        }
    }
    {
        // one file on a database that loses power and is recovered after every statement: the
        // statistics come back from checkpoints and logs, and plans and answers must not care
        SCOPED_TRACE(files.front());
        int skipped = 0;
        RunSqlFile(files.front(),
                   {.persistent = true, .unsupported = &skipped, .approximate_doubles = true});
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
        EXPECT_LE(skipped, 12);
    }
    std::cout << "[sqlfuzz] " << files.size() << " files, " << total_queries << " queries, "
              << unsupported / 2 << " NotImplemented per mode\n";
    EXPECT_LE(unsupported / 2, total_queries / 33)
        << "more than 3% of the generated queries need an "
           "unsupported feature: that is a finding";
}

} // namespace cdb
