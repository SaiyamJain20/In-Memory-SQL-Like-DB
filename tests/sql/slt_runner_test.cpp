// Runs the sqllogictest-style files in tests/sql/*.test against this engine. The expected results
// in them were produced by DuckDB (tools/gen_slt.py), so these are differential tests that do not
// need DuckDB at run time. See tools/gen_slt.py for the format.

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
            db_ = std::make_unique<Database>();
        }
        conn_ = std::make_unique<Connection>(*db_);
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
        }
        return r;
    }
    int restarts() const { return restarts_; }

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
};

void RunSqlFile(const std::string& path, bool persistent) {
    const std::vector<Block> blocks = ParseFile(path);
    ASSERT_FALSE(blocks.empty()) << path;
    Subject subject(persistent);
    int queries = 0;
    for (const Block& b : blocks) {
        const std::string where = fs::path(path).filename().string() + ":" + std::to_string(b.line);
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
            if (got != b.expected) {
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
    RunSqlFile(GetParam(), false);
}

TEST_P(SqlLogicPersistentTest, MatchesDuckDBExpectedResultsAcrossACrashAfterEveryStatement) {
    RunSqlFile(GetParam(), true);
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

} // namespace cdb
