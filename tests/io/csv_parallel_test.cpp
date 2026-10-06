// The parallel CSV loader must produce exactly what the serial one does: the same table, the same
// row groups, the same errors (with the same line numbers), and nothing published on failure.

#include "io/csv_reader.h"

#include "common/error.h"
#include "main/connection.h"
#include "main/database.h"
#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

constexpr idx_t kGroupRows = 2 * kVectorSize; // small row groups: many group boundaries

std::vector<ColumnDefinition> Schema(bool a_not_null = false) {
    return {{"a", LogicalType::Integer(), a_not_null},
            {"b", LogicalType::BigInt()},
            {"c", LogicalType::Double()},
            {"d", LogicalType::Date()},
            {"e", LogicalType::Varchar()},
            {"f", LogicalType::Boolean()}};
}

struct Style {
    char delimiter = ',';
    bool header = false;
    bool crlf = false;
    bool quoting = false; // quoted fields, with embedded delimiters, quotes and newlines
    bool blank_lines = false;
    bool trailing_delimiter = false; // dbgen style
};

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

// One record, without its terminator (it may contain newlines inside quotes).
std::string Record(Rng& rng, const Style& st) {
    const std::string d(1, st.delimiter);
    const auto number_or_null = [&](double nulls, const std::string& text) {
        return Chance(rng, nulls) ? std::string() : text;
    };
    std::string e;
    switch (RandBelow(rng, st.quoting ? 8 : 4)) {
    case 0:
        e = "word" + std::to_string(RandBelow(rng, 50));
        break;
    case 1:
        e = "a longer string that is out of line " + std::to_string(RandBelow(rng, 1000));
        break;
    case 2:
        e = ""; // NULL
        break;
    case 3:
        e = "x";
        break;
    case 4:
        e = Quote("with " + d + " delimiter");
        break;
    case 5:
        e = Quote("say \"hi\"");
        break;
    case 6:
        e = Quote(std::string("two\nlines") + (Chance(rng, 0.5) ? "\r\nand three" : ""));
        break;
    default:
        e = "\"\""; // quoted empty string
        break;
    }
    std::string r =
        number_or_null(0.1, std::to_string(static_cast<int>(RandBelow(rng, 2000)) - 1000)) + d +
        number_or_null(0.1, std::to_string(static_cast<int64_t>(rng()) >> 20)) + d +
        number_or_null(0.1, std::to_string(static_cast<int>(RandBelow(rng, 4000)) - 2000) + ".25") +
        d +
        number_or_null(0.1, "199" + std::to_string(RandBelow(rng, 9)) + "-0" +
                                std::to_string(1 + RandBelow(rng, 9)) + "-1" +
                                std::to_string(RandBelow(rng, 9))) +
        d + e + d + number_or_null(0.1, Chance(rng, 0.5) ? "true" : "0");
    if (st.trailing_delimiter) {
        r += d;
    }
    return r;
}

std::string Join(const std::vector<std::string>& records, const Style& st, Rng& rng) {
    const std::string eol = st.crlf ? "\r\n" : "\n";
    std::string out;
    if (st.header) {
        out += std::string("a") + st.delimiter + "b" + st.delimiter + "c" + st.delimiter + "d" +
               st.delimiter + "e" + st.delimiter + "f" + eol;
    }
    for (const std::string& r : records) {
        if (st.blank_lines && Chance(rng, 0.05)) {
            out += Chance(rng, 0.5) ? eol : "\r\n"; // a blank line (or a lone CR LF)
        }
        out += r + eol;
    }
    return out;
}

class TempFile {
  public:
    explicit TempFile(const std::string& content) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("cdb_csv_parallel_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++) + ".csv");
        std::ofstream out(path_, std::ios::binary);
        out << content;
    }
    ~TempFile() { std::filesystem::remove(path_); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string path() const { return path_.string(); }

  private:
    std::filesystem::path path_;
};

std::vector<std::vector<Value>> Contents(const Table& t) {
    const auto snap = t.Snapshot();
    TableScan scan(snap, test::AllColumns(*snap));
    return test::ScanAll(scan);
}

std::vector<idx_t> GroupSizes(const Table& t) {
    const auto snap = t.Snapshot();
    std::vector<idx_t> sizes;
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        sizes.push_back(snap->row_group(g).count());
    }
    return sizes;
}

CsvOptions Options(const Style& st, size_t chunk_bytes) {
    CsvOptions o;
    o.delimiter = st.delimiter;
    o.header = st.header;
    o.parallel_min_bytes = 0; // always take the parallel path (when the file allows it)
    o.parallel_chunk_bytes = chunk_bytes;
    return o;
}

// "" for success, else the error message.
std::string TryLoad(Table& t, const std::string& path, const CsvOptions& o, TaskScheduler* s,
                    ErrorCode* code = nullptr) {
    try {
        LoadCsvFile(t, path, o, s);
        return "";
    } catch (const Error& e) {
        if (code != nullptr) {
            *code = e.code();
        }
        return e.what();
    }
}

} // namespace

TEST(CsvParallel, LoadsExactlyWhatTheSerialLoaderLoads) {
    Rng rng(1);
    const idx_t counts[] = {0, 1, 2, 2047, 2048, 2049, 4095, 4096, 4097, 9000, 12288};
    int checked = 0;
    for (int iter = 0; iter < 60; iter++) {
        Style st;
        st.delimiter = Chance(rng, 0.5) ? ',' : '|';
        st.header = Chance(rng, 0.4);
        st.crlf = Chance(rng, 0.3);
        st.quoting = Chance(rng, 0.5);
        st.blank_lines = Chance(rng, 0.3);
        st.trailing_delimiter = Chance(rng, 0.3);
        const idx_t rows = counts[RandBelow(rng, std::size(counts))];
        std::vector<std::string> records;
        for (idx_t i = 0; i < rows; i++) {
            records.push_back(Record(rng, st));
        }
        std::string text = Join(records, st, rng);
        if (Chance(rng, 0.3) && !text.empty()) { // no terminator after the last record
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
        }
        const TempFile file(text);
        const size_t chunk_bytes = std::vector<size_t>{7, 16, 100, 1000, 0}[RandBelow(rng, 5)];
        const size_t threads = std::vector<size_t>{2, 3, 8}[RandBelow(rng, 3)];

        Table serial("t", Schema(), kGroupRows);
        ASSERT_EQ(TryLoad(serial, file.path(), Options(st, chunk_bytes), nullptr), "");
        Table parallel("t", Schema(), kGroupRows);
        TaskScheduler scheduler(threads);
        const uint64_t before = CsvParallelLoadCount();
        ASSERT_EQ(TryLoad(parallel, file.path(), Options(st, chunk_bytes), &scheduler), "")
            << "iteration " << iter;
        if (!text.empty()) {
            EXPECT_EQ(CsvParallelLoadCount(), before + 1) << "the parallel path must have run";
        }
        const std::string what = "iteration " + std::to_string(iter) + " rows " +
                                 std::to_string(rows) + " threads " + std::to_string(threads) +
                                 " chunk " + std::to_string(chunk_bytes) +
                                 (st.quoting ? " quoted" : "") + (st.crlf ? " crlf" : "") +
                                 (st.header ? " header" : "") + (st.blank_lines ? " blanks" : "");
        EXPECT_EQ(parallel.RowCount(), serial.RowCount()) << what;
        EXPECT_EQ(GroupSizes(parallel), GroupSizes(serial)) << what << ": same row groups";
        test::ExpectColumnsEqual(Contents(parallel), Contents(serial), what);
        if (::testing::Test::HasFailure()) {
            return;
        }
        checked++;
    }
    EXPECT_EQ(checked, 60);
}

TEST(CsvParallel, EdgeCaseInputs) {
    struct Case {
        const char* name;
        std::string text;
        bool header;
    };
    const std::vector<Case> cases = {
        {"only a header", "a,b,c,d,e,f\n", true},
        {"only blank lines", "\n\r\n\n", false},
        {"one record, no terminator", "1,2,3.5,1998-01-02,x,true", false},
        {"one record, CRLF", "1,2,3.5,1998-01-02,x,true\r\n", false},
        {"blank lines around records",
         "\n1,2,3.5,1998-01-02,x,true\n\n\n2,3,4.5,1998-01-03,y,false\n\n", false},
        {"a quoted newline right at a chunk boundary",
         "1,2,3.5,1998-01-02,\"a\nb\",true\n2,3,4.5,1998-01-03,\"c\r\nd\",false\n", false},
        {"dbgen trailing delimiters", "1,2,3.5,1998-01-02,x,true,\n2,3,4.5,1998-01-03,y,false,\n",
         false},
        {"quotes only in the last record",
         "1,2,3.5,1998-01-02,x,true\n2,3,4.5,1998-01-03,\"q,\"\"z\",false\n", false},
    };
    TaskScheduler scheduler(4);
    for (const Case& c : cases) {
        for (const size_t chunk_bytes : {size_t{1}, size_t{3}, size_t{10}, size_t{0}}) {
            const TempFile file(c.text);
            Style st;
            st.header = c.header;
            Table serial("t", Schema(), kGroupRows), parallel("t", Schema(), kGroupRows);
            ASSERT_EQ(TryLoad(serial, file.path(), Options(st, chunk_bytes), nullptr), "")
                << c.name;
            ASSERT_EQ(TryLoad(parallel, file.path(), Options(st, chunk_bytes), &scheduler), "")
                << c.name;
            test::ExpectColumnsEqual(Contents(parallel), Contents(serial),
                                     std::string(c.name) + " chunk " + std::to_string(chunk_bytes));
            EXPECT_EQ(GroupSizes(parallel), GroupSizes(serial)) << c.name;
        }
    }
}

namespace {

enum class Fault {
    TooFewFields,
    TooManyFields,
    BadInteger,
    BadDate,
    NullInNotNull,
    UnterminatedQuote,
    QuoteSwallowsTheRest
};

// Replaces record `at` with a broken one.
void Inject(std::vector<std::string>& records, size_t at, Fault fault, const Style& st) {
    const std::string d(1, st.delimiter);
    switch (fault) {
    case Fault::TooFewFields:
        records[at] = "1" + d + "2";
        break;
    case Fault::TooManyFields:
        records[at] = records[at] + d + "9" + d + "9";
        break;
    case Fault::BadInteger:
        records[at] = "notanumber" + records[at].substr(records[at].find(st.delimiter));
        break;
    case Fault::BadDate:
        records[at] = "1" + d + "2" + d + "3.5" + d + "1998-13-45" + d + "x" + d + "true";
        break;
    case Fault::NullInNotNull:
        records[at] = d + "2" + d + "3.5" + d + "1998-01-02" + d + "x" + d + "true";
        break;
    case Fault::UnterminatedQuote:
        records[at] =
            "1" + d + "2" + d + "3.5" + d + "1998-01-02" + d + "\"never closed" + d + "true";
        break;
    case Fault::QuoteSwallowsTheRest:
        records[at] = "1" + d + "2" + d + "3.5" + d + "1998-01-02" + d + "\"swallows" + d + "true";
        break;
    }
}

} // namespace

TEST(CsvParallel, ErrorsAreTheSerialLoadersErrorsAndNothingIsPublished) {
    Rng rng(2);
    const Fault faults[] = {
        Fault::TooFewFields,  Fault::TooManyFields,     Fault::BadInteger,          Fault::BadDate,
        Fault::NullInNotNull, Fault::UnterminatedQuote, Fault::QuoteSwallowsTheRest};
    for (int iter = 0; iter < 80; iter++) {
        Style st;
        st.delimiter = Chance(rng, 0.5) ? ',' : '|';
        st.header = Chance(rng, 0.4);
        st.crlf = Chance(rng, 0.3);
        st.quoting = Chance(rng, 0.5);
        st.blank_lines = Chance(rng, 0.3);
        const idx_t rows = 3 * kGroupRows + 100;
        std::vector<std::string> records;
        for (idx_t i = 0; i < rows; i++) {
            records.push_back(Record(rng, st));
            if (records.back().front() ==
                st.delimiter) { // keep column a non-NULL (the NOT NULL schema)
                records.back() = "7" + records.back();
            }
        }
        // one or two faults at random places (so the earliest, not the first-found, must win)
        const size_t nfaults = 1 + RandBelow(rng, 2);
        for (size_t f = 0; f < nfaults; f++) {
            Inject(records, RandBelow(rng, records.size()),
                   faults[RandBelow(rng, std::size(faults))], st);
        }
        const TempFile file(Join(records, st, rng));
        const size_t chunk_bytes = std::vector<size_t>{16, 1000, 0}[RandBelow(rng, 3)];
        const size_t threads = std::vector<size_t>{2, 4, 8}[RandBelow(rng, 3)];
        const bool not_null = Chance(rng, 0.5);

        Table serial("t", Schema(not_null), kGroupRows),
            parallel("t", Schema(not_null), kGroupRows);
        // rows that were already there must survive a failed load
        for (Table* t : {&serial, &parallel}) {
            DataChunk chunk;
            chunk.Initialize({LogicalType::Integer(), LogicalType::BigInt(), LogicalType::Double(),
                              LogicalType::Date(), LogicalType::Varchar(), LogicalType::Boolean()});
            for (idx_t r = 0; r < 5; r++) {
                chunk.SetValue(0, r, Value::Integer(static_cast<int32_t>(r)));
                chunk.SetValue(1, r, Value::BigInt(static_cast<int64_t>(r)));
                chunk.SetValue(2, r, Value::Double(static_cast<double>(r)));
                chunk.SetValue(3, r, Value::Date(date_t{10000}));
                chunk.SetValue(4, r, Value::Varchar("seed"));
                chunk.SetValue(5, r, Value::Boolean(true));
            }
            chunk.SetCardinality(5);
            t->Append(chunk);
        }

        TaskScheduler scheduler(threads);
        ErrorCode serial_code = ErrorCode::Internal, parallel_code = ErrorCode::Internal;
        const std::string serial_error =
            TryLoad(serial, file.path(), Options(st, chunk_bytes), nullptr, &serial_code);
        const std::string parallel_error =
            TryLoad(parallel, file.path(), Options(st, chunk_bytes), &scheduler, &parallel_code);
        EXPECT_EQ(parallel_error, serial_error) << "iteration " << iter;
        EXPECT_EQ(parallel_code, serial_code);
        if (!serial_error.empty()) {
            EXPECT_EQ(parallel.RowCount(), 5U) << "a failed load must publish nothing";
            test::ExpectColumnsEqual(Contents(parallel), Contents(serial), "after a failed load");
        } else {
            test::ExpectColumnsEqual(Contents(parallel), Contents(serial), "a load that succeeded");
        }
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(CsvParallel, TheParallelPathIsOnlyTakenForABigEnoughFileAndSeveralThreads) {
    Rng rng(4);
    std::vector<std::string> records;
    Style st;
    for (int i = 0; i < 300; i++) {
        records.push_back(Record(rng, st));
    }
    const TempFile file(Join(records, st, rng));
    TaskScheduler many(4), one(1);
    CsvOptions
        defaults; // 8 MiB (whatever the environment says): this small file is loaded serially
    defaults.delimiter = ',';
    defaults.parallel_min_bytes = size_t{8} << 20;
    Table a("t", Schema(), kGroupRows);
    uint64_t before = CsvParallelLoadCount();
    LoadCsvFile(a, file.path(), defaults, &many);
    EXPECT_EQ(CsvParallelLoadCount(), before) << "below the size threshold";

    CsvOptions forced = defaults;
    forced.parallel_min_bytes = 0;
    Table b("t", Schema(), kGroupRows);
    LoadCsvFile(b, file.path(), forced, nullptr);
    EXPECT_EQ(CsvParallelLoadCount(), before) << "no scheduler";
    Table c("t", Schema(), kGroupRows);
    LoadCsvFile(c, file.path(), forced, &one);
    EXPECT_EQ(CsvParallelLoadCount(), before) << "a one-thread scheduler";
    Table d("t", Schema(), kGroupRows);
    LoadCsvFile(d, file.path(), forced, &many);
    EXPECT_EQ(CsvParallelLoadCount(), before + 1);
    test::ExpectColumnsEqual(Contents(d), Contents(a), "same data either way");
}

TEST(CsvParallel, CopyThroughSqlLoadsTheSameDataOnOneAndSeveralThreads) {
    // Big enough (> 8 MiB) for COPY to take the parallel path with the default options.
    Rng rng(5);
    Style st;
    st.delimiter = '|';
    st.trailing_delimiter = true;
    std::string text;
    int rows = 0;
    while (text.size() < (size_t{9} << 20)) {
        text += Record(rng, st) + "\n";
        rows++;
    }
    const TempFile file(text);
    const auto run = [&](size_t threads) {
        Database db(threads);
        Connection conn(db);
        EXPECT_TRUE(
            conn.Query(
                    "CREATE TABLE t (a INTEGER, b BIGINT, c DOUBLE, d DATE, e VARCHAR, f BOOLEAN)")
                .ok());
        const QueryResult load = conn.Query("COPY t FROM '" + file.path() + "' (DELIMITER '|')");
        EXPECT_TRUE(load.ok()) << load.ToString();
        return conn
            .Query("SELECT count(*), sum(a), sum(b), sum(c), min(d), max(d), count(e), "
                   "min(e), max(e), count(f) FROM t")
            .ToString();
    };
    const uint64_t before = CsvParallelLoadCount();
    const std::string one = run(1);
    EXPECT_EQ(CsvParallelLoadCount(), before);
    const std::string four = run(4);
    EXPECT_EQ(CsvParallelLoadCount(), before + 1) << "COPY with 4 threads takes the parallel path";
    EXPECT_EQ(four, one);
    EXPECT_NE(one.find(std::to_string(rows)), std::string::npos);
}

TEST(CsvParallel, RandomHostileBytesGiveTheSameOutcomeAsTheSerialLoader) {
    // Quotes, delimiters, CR and LF in random order: unterminated quotes, quotes mid-field,
    // doubled quotes, blank lines, lone CRs. Both loaders must agree on rows or on the error. (In
    // debug builds the serial loader also asserts that its quote scanner and SplitRecord agree on
    // where every record ends.)
    Rng rng(6);
    const char alphabet[] = {'a', 'b', '1', ',', ',', '"', '"', '\n', '\n', '\r', ' '};
    const std::vector<ColumnDefinition> schema = {{"x", LogicalType::Varchar()},
                                                  {"y", LogicalType::Varchar()}};
    TaskScheduler scheduler(4);
    for (int iter = 0; iter < 600; iter++) {
        std::string text;
        const size_t length = RandBelow(rng, 120);
        for (size_t i = 0; i < length; i++) {
            text += alphabet[RandBelow(rng, std::size(alphabet))];
        }
        const TempFile file(text);
        CsvOptions options;
        options.delimiter = ',';
        options.header = Chance(rng, 0.3);
        options.parallel_min_bytes = 0;
        options.parallel_chunk_bytes = std::vector<size_t>{1, 2, 3, 5, 64}[RandBelow(rng, 5)];
        Table serial("t", schema, kGroupRows), parallel("t", schema, kGroupRows);
        const std::string serial_error = TryLoad(serial, file.path(), options, nullptr);
        const std::string parallel_error = TryLoad(parallel, file.path(), options, &scheduler);
        ASSERT_EQ(parallel_error, serial_error) << "input: " << ::testing::PrintToString(text);
        if (serial_error.empty()) {
            test::ExpectColumnsEqual(Contents(parallel), Contents(serial),
                                     "input: " + ::testing::PrintToString(text));
            ASSERT_EQ(GroupSizes(parallel), GroupSizes(serial));
        } else {
            ASSERT_EQ(parallel.RowCount(), 0U);
        }
    }
}

TEST(Csv, AStrayQuoteSwallowingALargeFileDoesNotTakeQuadraticTime) {
    // A record that spans 60,000 lines used to be re-split after every appended line (quadratic).
    std::string text = "1,\"opens a quote and never closes it\n";
    for (int i = 0; i < 60000; i++) {
        text += "line " + std::to_string(i) + ",still inside the quote\n";
    }
    const TempFile file(text);
    Table t("t", {{"x", LogicalType::Varchar()}, {"y", LogicalType::Varchar()}}, kGroupRows);
    const auto start = std::chrono::steady_clock::now();
    const std::string error = TryLoad(t, file.path(), CsvOptions{}, nullptr);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_NE(error.find("unterminated quoted field"), std::string::npos) << error;
    EXPECT_NE(error.find("line 1"), std::string::npos) << error;
    EXPECT_LT(seconds, 10.0) << "took " << seconds << " s";
}

} // namespace cdb
