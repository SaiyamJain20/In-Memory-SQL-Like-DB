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

struct Tpch {
    Database db;
    Connection conn{db};
    bool ready = false;
    std::string problem;

    Tpch() {
        if (!std::filesystem::exists(DataDir() + "/manifest.json")) {
            problem =
                "TPC-H data not generated (run: .venv/bin/python tools/tpch_data.py --sf 0.01)";
            return;
        }
        std::istringstream schema(ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/schema.sql"));
        const QueryResult created = conn.Query(schema.str());
        if (!created.ok()) {
            problem = "schema: " + created.error_message();
            return;
        }
        for (const char* t : kTables) {
            const QueryResult r = conn.Query(std::string("COPY ") + t + " FROM '" + DataDir() +
                                             "/" + t + ".csv' (DELIMITER '|', HEADER FALSE)");
            if (!r.ok()) {
                problem = std::string("loading ") + t + ": " + r.error_message();
                return;
            }
        }
        ready = true;
    }
};

Tpch& Shared() {
    static Tpch instance;
    return instance;
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

class TpchDifferential : public ::testing::TestWithParam<int> {};

TEST_P(TpchDifferential, MatchesDuckDB) {
    Tpch& t = Shared();
    if (!t.ready) {
        if (std::getenv("CDB_REQUIRE_TPCH")) {
            FAIL() << t.problem;
        }
        GTEST_SKIP() << t.problem;
    }
    char name[16];
    std::snprintf(name, sizeof name, "q%02d", GetParam());
    const std::string sql =
        ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/queries/" + name + ".sql");
    const QueryResult result = t.conn.Query(sql);
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

INSTANTIATE_TEST_SUITE_P(Queries, TpchDifferential,
                         ::testing::Values(1, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 19),
                         [](const ::testing::TestParamInfo<int>& param_info) {
                             return "Q" + std::to_string(param_info.param);
                         });

} // namespace cdb
