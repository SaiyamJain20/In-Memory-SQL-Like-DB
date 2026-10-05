// Differential test against DuckDB. tests/data/golden_expressions.tsv holds DuckDB's answer
// (type and value, or "ERROR") for thousands of constant expressions - operator grids over typed
// operands, cast matrices, three-valued logic, strings, dates, functions. Each one is run through
// this engine's parser, binder and scalar evaluator via `SELECT <expr>`. Regenerate the data
// with tools/gen_golden_expressions.py.
//
// Known, documented divergences from DuckDB are listed in kKnownDivergences (with the reason);
// every other expression must match exactly, and an entry there that starts matching fails the
// test so the list cannot rot.

#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace cdb {

namespace {

struct Golden {
    std::string expr, type, value;
};

std::vector<Golden> LoadGolden() {
    std::vector<Golden> out;
    std::ifstream in(std::string(CDB_SOURCE_DIR) + "/tests/data/golden_expressions.tsv");
    std::string line;
    while (std::getline(in, line)) {
        const size_t a = line.find('\t');
        const size_t b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        out.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
    }
    return out;
}

// Decodes a JSON string literal (as written by json.dumps(ensure_ascii=False)).
std::string JsonDecode(const std::string& s) {
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); i++) { // skip the surrounding quotes
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        switch (c) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            auto hex4 = [&](size_t at) { return std::stoul(s.substr(at, 4), nullptr, 16); };
            unsigned long cp = hex4(i + 1);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i + 1, 2, "\\u") == 0) { // surrogate pair
                const unsigned long lo = hex4(i + 3);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            out += c; // \" \\ \/
        }
    }
    return out;
}

bool ParseDouble(const std::string& s, double& out) {
    if (s == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (s == "inf" || s == "-inf") {
        out = s[0] == '-' ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity();
        return true;
    }
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

bool DoublesMatch(double a, double b) {
    if (std::isnan(a) || std::isnan(b))
        return std::isnan(a) && std::isnan(b);
    if (a == b)
        return true;
    return std::fabs(a - b) <= 1e-12 * std::max({1.0, std::fabs(a), std::fabs(b)});
}

// Returns "" when `r` agrees with the golden answer, otherwise a description of the difference.
std::string Compare(const Golden& g, const QueryResult& r) {
    if (g.type == "ERROR") {
        if (!r.ok())
            return "";
        return "DuckDB raises an error, we returned " + r.GetValue(0, 0).ToString();
    }
    if (!r.ok())
        return "unexpected error: " + r.error_message().substr(0, r.error_message().find('\n'));
    if (r.RowCount() != 1 || r.ColumnCount() != 1)
        return "expected one value";
    const Value v = r.GetValue(0, 0);
    if (g.type != "OTHER" && v.type().ToString() != g.type) {
        return "type " + v.type().ToString() + " but DuckDB says " + g.type + " (value " +
               v.ToString() + ")";
    }
    if (g.value == "NULL")
        return v.IsNull() ? "" : "expected NULL, got " + v.ToString();
    if (v.IsNull())
        return "got NULL, expected " + g.value;
    switch (v.type().id()) {
    case TypeId::Boolean:
        return (v.GetBoolean() ? "true" : "false") == g.value
                   ? ""
                   : "got " + v.ToString() + ", expected " + g.value;
    case TypeId::Integer:
    case TypeId::BigInt: {
        double expected;
        if (g.type == "OTHER" && ParseDouble(g.value, expected)) {
            const double got = v.type().id() == TypeId::Integer
                                   ? v.GetInteger()
                                   : static_cast<double>(v.GetBigInt());
            return DoublesMatch(got, expected) ? ""
                                               : "got " + v.ToString() + ", expected " + g.value;
        }
        return v.ToString() == g.value ? "" : "got " + v.ToString() + ", expected " + g.value;
    }
    case TypeId::Double: {
        double expected;
        if (!ParseDouble(g.value, expected))
            return "expected non-number " + g.value;
        return DoublesMatch(v.GetDouble(), expected)
                   ? ""
                   : "got " + v.ToString() + ", expected " + g.value;
    }
    case TypeId::Date:
        return v.ToString() == g.value ? "" : "got " + v.ToString() + ", expected " + g.value;
    case TypeId::Varchar: {
        const std::string expected =
            g.value.size() >= 2 && g.value[0] == '"' ? JsonDecode(g.value) : g.value;
        return v.GetVarchar() == expected
                   ? ""
                   : "got '" + v.GetVarchar() + "', expected '" + expected + "'";
    }
    }
    return "unhandled type";
}

// Expressions on which this engine deliberately differs from DuckDB. The value is the reason.
const std::map<std::string, std::string>& KnownDivergences() {
    static const std::map<std::string, std::string> kDivergences = {
        // DuckDB folds `FALSE AND <x>` before evaluating <x>; this engine folds constant casts
        // while binding, so an invalid constant conversion is reported even in a dead branch.
        {"FALSE AND CAST('x' AS INTEGER) = 1", "bind-time constant-cast errors in dead branches"},
        // DuckDB's DATE spans roughly +/-5.8 million years; this engine supports years 1..9999.
        {"DATE '0001-01-01' - 1", "DATE range is years 1..9999"},
        {"DATE '0001-01-01' - 365", "DATE range is years 1..9999"},
        {"DATE '9999-12-31' + 1", "DATE range is years 1..9999"},
        {"1 + DATE '9999-12-31'", "DATE range is years 1..9999"},
        {"DATE '9999-12-31' + 365", "DATE range is years 1..9999"},
    };
    return kDivergences;
}

} // namespace

TEST(GoldenExpressions, MatchDuckDB) {
    const std::vector<Golden> golden = LoadGolden();
    ASSERT_GT(golden.size(), 4000u) << "golden data missing";

    Database db;
    Connection conn(db);
    std::vector<std::string> mismatches;
    size_t compared = 0, errors_agreed = 0, fixed_divergences = 0;
    for (const Golden& g : golden) {
        const QueryResult r = conn.Query("SELECT " + g.expr);
        const std::string diff = Compare(g, r);
        compared++;
        if (g.type == "ERROR" && diff.empty())
            errors_agreed++;
        const auto known = KnownDivergences().find(g.expr);
        if (known != KnownDivergences().end()) {
            if (diff.empty()) {
                mismatches.push_back("[stale divergence entry, now matches] " + g.expr);
                fixed_divergences++;
            }
            continue;
        }
        if (!diff.empty())
            mismatches.push_back(g.expr + "   -> " + diff);
    }
    std::ostringstream report;
    report << mismatches.size() << " of " << compared << " expressions differ from DuckDB:\n";
    for (size_t i = 0; i < std::min<size_t>(mismatches.size(), 60); i++)
        report << "  " << mismatches[i] << "\n";
    EXPECT_TRUE(mismatches.empty()) << report.str();
    EXPECT_GT(errors_agreed, 100u); // both sides must also agree on what is an error
    (void)fixed_divergences;
}

} // namespace cdb
