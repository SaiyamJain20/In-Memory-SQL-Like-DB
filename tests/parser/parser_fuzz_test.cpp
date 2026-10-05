// Deterministic mutational fuzzing of the parser (runs in every CI configuration, notably under
// ASan/UBSan). For every input the parser must either
//   * succeed, in which case printing the AST and parsing the printout again must reproduce the
//     same text (print/parse fixpoint) - this catches printer/parser disagreements, or
//   * throw cdb::Error(Syntax | NotImplemented) with an in-range position.
// Anything else (crash, hang, other exception type, bad position) fails. A libFuzzer harness
// with the same checks lives in fuzz/parser_fuzz.cpp for open-ended runs.

#include "parser/parser.h"
#include "parser/token.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cdb {

namespace {

std::vector<std::string> SeedCorpus() {
    std::vector<std::string> seeds = {
        "SELECT 1",
        "SELECT a, b + 1 AS c FROM t WHERE a > 1 AND (b < 2 OR NOT c) GROUP BY a HAVING sum(b) > 3 "
        "ORDER BY a DESC NULLS LAST LIMIT 10 OFFSET 2",
        "SELECT * FROM a JOIN b ON a.x = b.x LEFT OUTER JOIN c USING (k) WHERE a.y BETWEEN 1 AND 5",
        "SELECT CASE WHEN a > 1 THEN 'x' ELSE 'y' END, CAST(a AS BIGINT), a::double FROM t",
        "SELECT EXTRACT(year FROM d), substring(s FROM 1 FOR 2), count(DISTINCT a) FROM t",
        "SELECT a FROM t WHERE a IN (1, 2) AND b NOT IN (SELECT c FROM u) AND EXISTS (SELECT 1)",
        "SELECT x FROM (SELECT a AS x FROM t) AS s (x) WHERE x > date '1998-12-01' - interval '90' "
        "day",
        "SELECT \"Weird\", 'it''s', 1.5e3, -5, TRUE, NULL FROM \"T 2\" AS q",
        "CREATE TABLE t (a INT NOT NULL, b VARCHAR(25), c DECIMAL(15,2), d DATE)",
        "INSERT INTO t (a, b) VALUES (1, 'x'), (2, NULL)",
        "INSERT INTO t SELECT a FROM u",
        "COPY t FROM 'f.csv' (DELIMITER '|', HEADER)",
        "EXPLAIN ANALYZE SELECT * FROM t",
        "DROP TABLE IF EXISTS t; SELECT 1; ",
    };
    const std::filesystem::path dir = std::filesystem::path(CDB_SOURCE_DIR) / "bench/tpch/queries";
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::ifstream in(entry.path());
        std::stringstream ss;
        ss << in.rdbuf();
        seeds.push_back(ss.str());
    }
    return seeds;
}

const std::vector<std::string>& Dictionary() {
    static const std::vector<std::string> kWords = {
        "SELECT",  "FROM",      "WHERE",    "GROUP",   "BY",    "HAVING",     "ORDER",  "LIMIT",
        "OFFSET",  "AS",        "AND",      "OR",      "NOT",   "NULL",       "TRUE",   "FALSE",
        "IS",      "IN",        "BETWEEN",  "LIKE",    "CASE",  "WHEN",       "THEN",   "ELSE",
        "END",     "CAST",      "DISTINCT", "JOIN",    "LEFT",  "RIGHT",      "FULL",   "CROSS",
        "ON",      "USING",     "CREATE",   "TABLE",   "DROP",  "IF",         "EXISTS", "INSERT",
        "INTO",    "VALUES",    "COPY",     "EXPLAIN", "UNION", "WITH",       "date",   "interval",
        "extract", "substring", "year",     "day",     "a",     "b",          "t",      "x1",
        "\"Q\"",   "'str'",     "'it''s'",  "0",       "42",    "2147483648", "1.5",    ".5",
        "1e9",     "(",         ")",        ",",       ";",     ".",          "*",      "+",
        "-",       "/",         "%",        "=",       "<",     ">",          "<=",     ">=",
        "<>",      "!=",        "||",       "::",      "--",    "/*",         "*/",     "'",
        "\"",      "\n",        " ",
    };
    return kWords;
}

std::string Mutate(const std::string& input, test::Rng& rng) {
    std::string s = input;
    const int rounds = 1 + static_cast<int>(test::RandBelow(rng, 3));
    for (int r = 0; r < rounds; r++) {
        if (s.empty())
            s = "SELECT 1";
        const size_t at = test::RandBelow(rng, s.size());
        switch (test::RandBelow(rng, 7)) {
        case 0: // flip a byte
            s[at] = static_cast<char>(rng());
            break;
        case 1: { // delete a range
            const size_t len = 1 + test::RandBelow(rng, std::min<size_t>(20, s.size() - at));
            s.erase(at, len);
            break;
        }
        case 2: { // duplicate a range
            const size_t len = 1 + test::RandBelow(rng, std::min<size_t>(30, s.size() - at));
            s.insert(at, s.substr(at, len));
            break;
        }
        case 3: // insert a dictionary word
            s.insert(at, " " + Dictionary()[test::RandBelow(rng, Dictionary().size())] + " ");
            break;
        case 4: // truncate
            s.resize(at);
            break;
        case 5: { // replace a range with a dictionary word
            const size_t len = 1 + test::RandBelow(rng, std::min<size_t>(8, s.size() - at));
            s.replace(at, len, Dictionary()[test::RandBelow(rng, Dictionary().size())]);
            break;
        }
        default: { // swap two ranges
            const size_t other = test::RandBelow(rng, s.size());
            std::swap(s[at], s[other]);
            break;
        }
        }
    }
    return s;
}

std::string TokenSoup(test::Rng& rng) {
    std::string s;
    const size_t n = 1 + test::RandBelow(rng, 25);
    for (size_t i = 0; i < n; i++) {
        s += Dictionary()[test::RandBelow(rng, Dictionary().size())];
        s += ' ';
    }
    return s;
}

struct Outcome {
    size_t parsed = 0, syntax = 0, not_implemented = 0;
};

// Applies the contract above to one input; returns false (after recording a failure) on violation.
bool CheckOne(const std::string& sql, Outcome& out) {
    try {
        auto stmts = ParseStatements(sql);
        std::string printed;
        for (const auto& s : stmts)
            printed += s->ToString() + "; ";
        std::string again;
        try {
            for (const auto& s : ParseStatements(printed))
                again += s->ToString() + "; ";
        } catch (const Error& e) {
            // The printed form of something we parsed must itself be parseable.
            ADD_FAILURE() << "printed SQL does not re-parse\n input:   " << sql
                          << "\n printed: " << printed << "\n error:   " << e.what();
            return false;
        }
        if (printed != again) {
            ADD_FAILURE() << "print/parse is not a fixpoint\n input:   " << sql
                          << "\n printed: " << printed << "\n again:   " << again;
            return false;
        }
        out.parsed++;
    } catch (const Error& e) {
        if (e.code() != ErrorCode::Syntax && e.code() != ErrorCode::NotImplemented) {
            ADD_FAILURE() << "unexpected error code " << ErrorCodeName(e.code()) << " for: " << sql;
            return false;
        }
        if (e.position().has_value() && *e.position() > sql.size()) {
            ADD_FAILURE() << "position " << *e.position() << " beyond input of " << sql.size()
                          << " bytes: " << sql;
            return false;
        }
        (e.code() == ErrorCode::Syntax ? out.syntax : out.not_implemented)++;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "unexpected exception type: " << e.what() << " for: " << sql;
        return false;
    }
    return true;
}

} // namespace

TEST(ParserFuzz, MutatedRealQueriesNeverBreakTheContract) {
    const auto seeds = SeedCorpus();
    test::Rng rng(20261006);
    Outcome out;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 40000; i++) {
        const std::string sql = Mutate(seeds[test::RandBelow(rng, seeds.size())], rng);
        if (!CheckOne(sql, out))
            return;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    // The mutator must exercise both outcomes, otherwise it proves nothing.
    EXPECT_GT(out.parsed, 2000u);
    EXPECT_GT(out.syntax, 2000u);
    EXPECT_GT(out.not_implemented, 20u);
    RecordProperty("fuzz_ms", static_cast<int>(ms));
}

TEST(ParserFuzz, TokenSoupNeverBreaksTheContract) {
    test::Rng rng(77);
    Outcome out;
    for (int i = 0; i < 40000; i++) {
        if (!CheckOne(TokenSoup(rng), out))
            return;
    }
    EXPECT_GT(out.syntax, 10000u);
}

TEST(ParserFuzz, EveryPrefixAndSuffixOfRealQueriesIsHandled) {
    const auto seeds = SeedCorpus();
    Outcome out;
    for (const std::string& seed : seeds) {
        // every prefix of the first 400 bytes, and the matching suffixes
        for (size_t n = 0; n <= std::min<size_t>(seed.size(), 400); n++) {
            if (!CheckOne(seed.substr(0, n), out))
                return;
            if (!CheckOne(seed.substr(seed.size() - n), out))
                return;
        }
    }
    EXPECT_GT(out.parsed, 20u);
}

TEST(ParserFuzz, RawRandomBytes) {
    test::Rng rng(5);
    Outcome out;
    for (int i = 0; i < 20000; i++) {
        std::string s(test::RandBelow(rng, 60), 0);
        for (char& c : s)
            c = static_cast<char>(rng());
        if (!CheckOne(s, out))
            return;
    }
    EXPECT_EQ(out.parsed, 0u + out.parsed); // outcome mix is irrelevant here; only the contract
}

} // namespace cdb
