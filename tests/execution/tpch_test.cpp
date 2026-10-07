// Differential test against DuckDB on TPC-H. tools/tpch_data.py generates the data and DuckDB's
// answers (data/tpch-sf0.01/...); every supported query is run here through the SQL front end,
// optimizer and vectorized executor, and its rows must equal DuckDB's: integers, strings and dates
// exactly, doubles within a relative tolerance (this engine stores DECIMAL as DOUBLE, ADR 0003),
// and in the same order.
//
// The data is generated, not checked in. If it is missing the tests skip, unless CDB_REQUIRE_TPCH
// is set (tools/verify.sh and the CI test jobs set it), in which case they fail. CDB_TPCH_SF
// selects the scale factor (default 0.01; the roadmap's exit criteria are SF0.1 and SF1, run by
// hand).

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cdb {

namespace {

const char* const kTables[] = {"nation",   "region",   "part",   "supplier",
                               "partsupp", "customer", "orders", "lineitem"};

// The scale factor under test: CDB_TPCH_SF (e.g. 0.1 or 1; data from tools/tpch_data.py --sf N),
// default 0.01, the one the gate and CI use.
std::string DataDir() {
    const char* sf = std::getenv("CDB_TPCH_SF");
    return std::string(CDB_SOURCE_DIR) + "/data/tpch-sf" + (sf ? sf : "0.01");
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// RFC 4180 with '|' as the delimiter (DuckDB quotes fields that contain special characters).
std::vector<std::vector<std::string>> ReadPipeRows(const std::string& path) {
    const std::string text = ReadFile(path);
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> cells;
    std::string cell;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); i++) {
        const char ch = text[i];
        if (quoted) {
            if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                cell += '"';
                i++;
            } else if (ch == '"') {
                quoted = false;
            } else {
                cell += ch;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '|') {
            cells.push_back(std::move(cell));
            cell.clear();
        } else if (ch == '\n') {
            cells.push_back(std::move(cell));
            cell.clear();
            rows.push_back(std::move(cells));
            cells.clear();
        } else {
            cell += ch;
        }
    }
    if (!cell.empty() || !cells.empty()) {
        cells.push_back(std::move(cell));
        rows.push_back(std::move(cells));
    }
    return rows;
}

// Where the data the queries run on lives: loaded straight into memory; loaded into a persistent
// database that is then closed (which checkpoints) and reopened from the checkpoint file; or loaded
// into one whose log is all there is, so that reopening replays the COPY statements from the log.
enum class Source { Memory = 0, Checkpoint = 1, Log = 2 };

const char* SourceName(Source s) {
    switch (s) {
    case Source::Memory:
        return "Memory";
    case Source::Checkpoint:
        return "ReopenedFromCheckpoint";
    case Source::Log:
        return "ReopenedFromLog";
    }
    return "?";
}

struct Tpch {
    std::shared_ptr<MemoryFileSystem> disk;
    std::unique_ptr<Database> db;
    std::unique_ptr<Connection> conn;
    bool ready = false;
    std::string problem;

    explicit Tpch(Source source) {
        if (!std::filesystem::exists(DataDir() + "/manifest.json")) {
            problem =
                "TPC-H data not generated (run: .venv/bin/python tools/tpch_data.py --sf 0.01)";
            return;
        }
        DatabaseOptions options;
        if (source != Source::Memory) {
            disk = std::make_shared<MemoryFileSystem>();
            options.storage.fs = disk;
            options.storage.checkpoint_on_close = source == Source::Checkpoint;
            options.storage.checkpoint_wal_bytes = 0;
        }
        Open(source, options);
        std::istringstream schema(ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/schema.sql"));
        const QueryResult created = conn->Query(schema.str());
        if (!created.ok()) {
            problem = "schema: " + created.error_message();
            return;
        }
        for (const char* t : kTables) {
            const QueryResult r = conn->Query(std::string("COPY ") + t + " FROM '" + DataDir() +
                                              "/" + t + ".csv' (DELIMITER '|', HEADER FALSE)");
            if (!r.ok()) {
                problem = std::string("loading ") + t + ": " + r.error_message();
                return;
            }
        }
        if (source != Source::Memory) {
            conn.reset();
            db.reset(); // closing: a checkpoint, or (Log) nothing but the fsynced log
            Open(source, options);
            if (db->storage()->recovery().had_checkpoint != (source == Source::Checkpoint)) {
                problem = "the database was not recovered the way this test expects";
                return;
            }
        }
        ready = true;
    }

    void Open(Source source, const DatabaseOptions& options) {
        db = source == Source::Memory ? std::make_unique<Database>()
                                      : std::make_unique<Database>("/db", options);
        conn = std::make_unique<Connection>(*db);
    }
};

Tpch& Shared(Source source) {
    static Tpch memory(Source::Memory);
    static Tpch checkpoint(Source::Checkpoint);
    static Tpch log(Source::Log);
    return source == Source::Memory ? memory : (source == Source::Checkpoint ? checkpoint : log);
}

bool CellMatches(const Value& got, const std::string& want, std::string& why) {
    if (got.IsNull()) {
        why = "got NULL";
        return want.empty(); // DuckDB writes NULL as an empty field
    }
    switch (got.type().id()) {
    case TypeId::Double: {
        if (want.empty()) {
            why = "expected NULL";
            return false;
        }
        const double w = std::strtod(want.c_str(), nullptr);
        const double g = got.GetDouble();
        const double tol = 1e-9 * std::max(1.0, std::fabs(w));
        if (std::fabs(g - w) <= tol) {
            return true;
        }
        why = "double " + got.ToString() + " vs " + want;
        return false;
    }
    case TypeId::Varchar:
        why = "string '" + got.GetVarchar() + "' vs '" + want + "'";
        return got.GetVarchar() == want;
    default:
        why = got.ToString() + " vs " + want;
        return got.ToString() == want;
    }
}

} // namespace

class TpchDifferential : public ::testing::TestWithParam<std::tuple<Source, int>> {};

TEST_P(TpchDifferential, MatchesDuckDB) {
    const auto [source, query] = GetParam();
    Tpch& t = Shared(source);
    if (!t.ready) {
        if (std::getenv("CDB_REQUIRE_TPCH")) {
            FAIL() << t.problem;
        }
        GTEST_SKIP() << t.problem;
    }
    char name[16];
    std::snprintf(name, sizeof name, "q%02d", query);
    const std::string sql =
        ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/queries/" + name + ".sql");
    const QueryResult result = t.conn->Query(sql);
    ASSERT_TRUE(result.ok()) << name << ": " << result.error_message();
    const auto expected = ReadPipeRows(DataDir() + "/expected/" + name + ".csv");
    ASSERT_EQ(result.RowCount(), expected.size()) << name << " row count";
    for (idx_t r = 0; r < result.RowCount(); r++) {
        ASSERT_EQ(expected[r].size(), result.ColumnCount()) << name << " column count";
        for (idx_t c = 0; c < result.ColumnCount(); c++) {
            std::string why;
            ASSERT_TRUE(CellMatches(result.GetValue(c, r), expected[r][c], why))
                << name << " row " << r << " column " << c << " (" << result.names()[c]
                << "): " << why;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Queries, TpchDifferential,
    ::testing::Combine(::testing::Values(Source::Memory, Source::Checkpoint, Source::Log),
                       ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                         18, 19, 20, 21, 22)),
    [](const ::testing::TestParamInfo<std::tuple<Source, int>>& param_info) {
        return std::string(SourceName(std::get<0>(param_info.param))) + "_Q" +
               std::to_string(std::get<1>(param_info.param));
    });

} // namespace cdb
