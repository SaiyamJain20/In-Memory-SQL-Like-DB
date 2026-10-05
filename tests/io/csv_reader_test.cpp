#include "io/csv_reader.h"

#include "common/error.h"
#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cdb {

namespace {

using test::ScanAll;

std::vector<ColumnDefinition> Schema() {
    return {{"id", LogicalType::Integer()},
            {"name", LogicalType::Varchar()},
            {"score", LogicalType::Double()}};
}

idx_t Load(Table& t, const std::string& csv, CsvOptions opt = {}) {
    std::istringstream in(csv);
    return LoadCsv(t, in, opt);
}

std::vector<std::vector<Value>> Rows(Table& t) {
    auto snap = t.Snapshot();
    TableScan scan(snap, test::AllColumns(*snap));
    return ScanAll(scan);
}

std::string ErrorMessage(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const Error& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected an error";
    return "";
}

} // namespace

TEST(Csv, BasicRows) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1,alice,1.5\n2,bob,-2\n3,carol,1e3\n"), 3u);
    auto rows = Rows(t);
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][2].GetInteger(), 3);
    EXPECT_EQ(rows[1][1].GetVarchar(), "bob");
    EXPECT_EQ(rows[2][0].GetDouble(), 1.5);
    EXPECT_EQ(rows[2][1].GetDouble(), -2.0);
    EXPECT_EQ(rows[2][2].GetDouble(), 1000.0);
}

TEST(Csv, NoTrailingNewlineCrlfAndBlankLines) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1,a,1\r\n\r\n2,b,2\r\n\n3,c,3"), 3u);
    EXPECT_EQ(Rows(t)[1][2].GetVarchar(), "c");
}

TEST(Csv, CrlfLineEndingsNeverLeakIntoTextFields) {
    // The last column is VARCHAR: a stray '\r' would be stored verbatim (numeric columns would
    // hide it, because their parsers trim whitespace).
    Table t("t", {{"id", LogicalType::Integer()}, {"word", LogicalType::Varchar()}});
    EXPECT_EQ(Load(t, "1,alpha\r\n2,beta\r\n3,\"quoted\"\r\n4,gamma"), 4u);
    auto rows = Rows(t);
    EXPECT_EQ(rows[1][0].GetVarchar(), "alpha");
    EXPECT_EQ(rows[1][1].GetVarchar(), "beta");
    EXPECT_EQ(rows[1][2].GetVarchar(), "quoted");
    EXPECT_EQ(rows[1][3].GetVarchar(), "gamma");
}

TEST(Csv, HeaderIsSkippedOnlyWhenRequested) {
    Table with("t", Schema());
    EXPECT_EQ(Load(with, "id,name,score\n1,a,1\n", {',', true}), 1u);
    Table without("t", Schema());
    EXPECT_NE(ErrorMessage([&] { Load(without, "id,name,score\n1,a,1\n"); }).find("line 1"),
              std::string::npos); // "id" is not an integer
    Table header_only("t", Schema());
    EXPECT_EQ(Load(header_only, "id,name,score\n", {',', true}), 0u);
    Table empty("t", Schema());
    EXPECT_EQ(Load(empty, "", {',', true}), 0u);
    EXPECT_EQ(Load(empty, ""), 0u);
}

TEST(Csv, CustomDelimiters) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1|a,b|1.5\n2|\"c|d\"|2\n", {'|', false}), 2u);
    auto rows = Rows(t);
    EXPECT_EQ(rows[1][0].GetVarchar(), "a,b");
    EXPECT_EQ(rows[1][1].GetVarchar(), "c|d");
    Table tab("t", Schema());
    EXPECT_EQ(Load(tab, "1\tx y\t3\n", {'\t', false}), 1u);
    EXPECT_EQ(Rows(tab)[1][0].GetVarchar(), "x y");
}

TEST(Csv, QuotingEscapesAndEmbeddedNewlines) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1,\"has,comma\",1\n"
                      "2,\"say \"\"hi\"\"\",2\n"
                      "3,\"two\nlines\",3\n"
                      "4,\"crlf\r\ninside\",4\n"
                      "5,\"\",5\n"),
              5u);
    auto rows = Rows(t);
    EXPECT_EQ(rows[1][0].GetVarchar(), "has,comma");
    EXPECT_EQ(rows[1][1].GetVarchar(), "say \"hi\"");
    EXPECT_EQ(rows[1][2].GetVarchar(), "two\nlines");
    EXPECT_EQ(rows[1][3].GetVarchar(), "crlf\r\ninside");
    EXPECT_FALSE(rows[1][4].IsNull());
    EXPECT_EQ(rows[1][4].GetVarchar(), ""); // quoted empty = empty string
}

TEST(Csv, QuotesInTheMiddleOfAnUnquotedFieldAreLiteral) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1,5\" pipe,2\n2,say \"x\",3\n"), 2u);
    auto rows = Rows(t);
    EXPECT_EQ(rows[1][0].GetVarchar(), "5\" pipe");
    EXPECT_EQ(rows[1][1].GetVarchar(), "say \"x\"");
}

TEST(Csv, EmptyUnquotedFieldsAreNull) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1,,\n,x,2\n"), 2u);
    auto rows = Rows(t);
    EXPECT_FALSE(rows[0][0].IsNull());
    EXPECT_TRUE(rows[1][0].IsNull()); // empty varchar -> NULL when unquoted
    EXPECT_TRUE(rows[2][0].IsNull()); // empty double -> NULL
    EXPECT_TRUE(rows[0][1].IsNull());
    EXPECT_EQ(rows[1][1].GetVarchar(), "x");
    // quoted empty is NULL for non-text columns
    Table q("t", Schema());
    EXPECT_EQ(Load(q, "\"\",a,1\n"), 1u);
    EXPECT_TRUE(Rows(q)[0][0].IsNull());
}

TEST(Csv, TrailingDelimiterIsToleratedOnce) {
    Table t("t", Schema());
    EXPECT_EQ(Load(t, "1|a|1.5|\n2|b|2.5|\n", {'|', false}), 2u);
    EXPECT_NE(ErrorMessage([&] {
                  Load(t, "3|c|1|x\n", {'|', false});
              }).find("expected 3 fields but found 4"),
              std::string::npos);
    EXPECT_EQ(t.RowCount(), 2u);
}

TEST(Csv, AllTypes) {
    Table t("t", test::AllTypesSchema());
    EXPECT_EQ(Load(t, "true,1,9000000000,1.25,1998-12-01,hello\n"
                      "false,-2147483648,-9223372036854775808,-0.5,0001-01-01,\"x\"\n"
                      "t,  42 ,7,inf,1970-1-5,\n"
                      "0,2,3,nan,2000-02-29,é\n"),
              4u);
    auto rows = Rows(t);
    EXPECT_EQ(rows[0][0].GetBoolean(), true);
    EXPECT_EQ(rows[0][1].GetBoolean(), false);
    EXPECT_EQ(rows[0][2].GetBoolean(), true);  // 't'
    EXPECT_EQ(rows[0][3].GetBoolean(), false); // '0'
    EXPECT_EQ(rows[1][1].GetInteger(), -2147483648);
    EXPECT_EQ(rows[2][0].GetBigInt(), 9000000000LL);
    EXPECT_EQ(rows[2][1].GetBigInt(), std::numeric_limits<int64_t>::min());
    EXPECT_TRUE(rows[5][2].IsNull()); // the unquoted empty text field
    EXPECT_EQ(rows[3][2].GetDouble(), std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(rows[3][3].GetDouble()));
    EXPECT_EQ(rows[4][0].ToString(), "1998-12-01");
    EXPECT_EQ(rows[4][2].ToString(), "1970-01-05");
    EXPECT_EQ(rows[5][3].GetVarchar(), "é");
    EXPECT_EQ(rows[1][1].GetInteger(), -2147483648);
    EXPECT_EQ(rows[1][2].GetInteger(),
              42); // whitespace-padded integers go through the general path
}

TEST(Csv, ErrorsNameTheLineAndColumn) {
    {
        Table t("t", Schema());
        const std::string msg = ErrorMessage([&] { Load(t, "1,a,1\n2,b,oops\n"); });
        EXPECT_NE(msg.find("line 2"), std::string::npos) << msg;
        EXPECT_NE(msg.find("column \"score\""), std::string::npos) << msg;
        EXPECT_NE(msg.find("oops"), std::string::npos) << msg;
    }
    {
        Table t("t", Schema());
        const std::string msg = ErrorMessage([&] { Load(t, "1,a,1\n2,b\n"); });
        EXPECT_NE(msg.find("line 2"), std::string::npos) << msg;
        EXPECT_NE(msg.find("expected 3 fields but found 2"), std::string::npos) << msg;
    }
    {
        Table t("t", Schema());
        const std::string msg = ErrorMessage([&] { Load(t, "99999999999,a,1\n"); });
        EXPECT_NE(msg.find("column \"id\""), std::string::npos) << msg;
    }
    {
        Table t("t", Schema());
        // a multi-line quoted field counts lines correctly: the bad row starts on line 4
        const std::string msg = ErrorMessage([&] { Load(t, "1,\"a\nb\nc\",1\nbad,x,1\n"); });
        EXPECT_NE(msg.find("line 4"), std::string::npos) << msg;
    }
    {
        Table t("t", Schema());
        const std::string msg = ErrorMessage([&] { Load(t, "1,\"never closed,1\n"); });
        EXPECT_NE(msg.find("unterminated"), std::string::npos) << msg;
    }
}

TEST(Csv, FailedLoadsLeaveTheTableUntouched) {
    // The bad row comes after several full chunks have been staged: nothing may leak out.
    Table t("t", Schema(), 2 * kVectorSize);
    ASSERT_EQ(Load(t, "7,seed,7\n"), 1u);
    std::string csv;
    for (int i = 0; i < 10000; i++)
        csv += std::to_string(i) + ",name" + std::to_string(i) + ",1\n";
    csv += "oops,x,1\n";
    EXPECT_THROW(Load(t, csv), Error);
    EXPECT_EQ(t.RowCount(), 1u);
    EXPECT_EQ(Rows(t)[1][0].GetVarchar(), "seed");
    // and the table is still fully usable afterwards
    EXPECT_EQ(Load(t, "8,after,8\n"), 1u);
    EXPECT_EQ(t.RowCount(), 2u);
}

TEST(Csv, NotNullViolationsAreRejectedAtomically) {
    Table t("t", {{"id", LogicalType::Integer(), true}, {"v", LogicalType::Integer()}});
    EXPECT_THROW(Load(t, "1,2\n,3\n"), Error); // empty id = NULL in a NOT NULL column
    EXPECT_EQ(t.RowCount(), 0u);
    EXPECT_EQ(Load(t, "1,\n2,3\n"), 2u);
}

TEST(Csv, AppendsToExistingRowsInOrder) {
    Table t("t", Schema(), 2 * kVectorSize);
    Load(t, "1,a,1\n2,b,2\n");
    Load(t, "3,c,3\n");
    auto rows = Rows(t);
    EXPECT_EQ(rows[0][0].GetInteger(), 1);
    EXPECT_EQ(rows[0][1].GetInteger(), 2);
    EXPECT_EQ(rows[0][2].GetInteger(), 3);
}

TEST(Csv, LargeRandomRoundTripAcrossManyChunksAndGroups) {
    test::Rng rng(31337);
    Table t("t", test::AllTypesSchema(), 2 * kVectorSize);
    std::vector<std::vector<Value>> expect(6);
    std::ostringstream csv;
    auto quote = [](std::string s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"')
                out += '"';
            out += c;
        }
        return out + "\"";
    };
    const int kRows = 30000;
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < 6; c++) {
            Value v = test::RandomValue(rng, t.schema()[c].type, 0.15);
            if (v.type().id() == TypeId::Varchar && !v.IsNull()) {
                // text fields: avoid NUL bytes (not representable in a text file) and the empty
                // string (which would need quoting) by quoting everything
                std::string s;
                for (char ch : v.GetVarchar())
                    s += ch == '\0' ? 'z' : ch;
                v = Value::Varchar(s);
            }
            expect[c].push_back(v);
            if (c > 0)
                csv << ',';
            if (v.IsNull())
                continue; // unquoted empty = NULL
            if (v.type().id() == TypeId::Varchar) {
                csv << quote(v.GetVarchar());
            } else if (v.type().id() == TypeId::Double) {
                // %.17g round-trips exactly; spell the special values the loader accepts
                const double d = v.GetDouble();
                if (std::isnan(d))
                    csv << "nan";
                else if (std::isinf(d))
                    csv << (d < 0 ? "-inf" : "inf");
                else {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "%.17g", d);
                    csv << buf;
                }
            } else {
                csv << v.ToString();
            }
        }
        csv << '\n';
    }
    EXPECT_EQ(Load(t, csv.str()), static_cast<idx_t>(kRows));
    auto got = Rows(t);
    for (int c = 0; c < 6; c++) {
        ASSERT_EQ(got[c].size(), expect[c].size());
        for (size_t r = 0; r < expect[c].size(); r++) {
            Value e = expect[c][r];
            // the loader maps -0.0 through text; compare doubles by value, others exactly
            if (!test::BitIdentical(got[c][r], e) &&
                !(e.type().id() == TypeId::Double && !e.IsNull() && !got[c][r].IsNull() &&
                  Value::Compare(got[c][r], e) == 0)) {
                FAIL() << "column " << c << " row " << r << ": got " << got[c][r].ToString()
                       << " expected " << e.ToString();
            }
        }
    }
}

TEST(Csv, FileInterface) {
    const auto path = std::filesystem::temp_directory_path() / "cdb_csv_test_file.csv";
    {
        std::ofstream out(path);
        out << "id,name,score\n1,x,1.5\n";
    }
    Table t("t", Schema());
    EXPECT_EQ(LoadCsvFile(t, path.string(), {',', true}), 1u);
    std::filesystem::remove(path);
    const std::string msg = ErrorMessage([&] { LoadCsvFile(t, path.string(), {}); });
    EXPECT_NE(msg.find("cannot open file"), std::string::npos) << msg;
    try {
        LoadCsvFile(t, path.string(), {});
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Io);
    }
}

} // namespace cdb
