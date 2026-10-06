#include "io/csv_reader.h"

#include "common/error.h"
#include "types/cast.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define CDB_CSV_HAS_MMAP 1
#endif

namespace cdb {

namespace {

struct Field {
    std::string text;
    bool quoted = false;
};

[[noreturn]] void RowError(idx_t line, const std::string& column, const std::string& detail) {
    std::string where = "CSV error at line " + std::to_string(line);
    if (!column.empty())
        where += ", column \"" + column + "\"";
    throw Error(ErrorCode::Execution, where + ": " + detail);
}

// Splits one logical record (which may contain embedded newlines) into fields. Returns false if
// the record ends inside a quoted field, i.e. it continues on the next line. A quote is
// structural only at the start of a field (or doubled inside a quoted one); elsewhere it is an
// ordinary character, so `5" pipe` is just text.
bool SplitRecord(const std::string& record, char delimiter, std::vector<Field>& fields) {
    fields.clear();
    Field current;
    bool in_quotes = false;
    for (size_t i = 0; i < record.size(); i++) {
        const char c = record[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < record.size() && record[i + 1] == '"') {
                    current.text += '"';
                    i++;
                } else {
                    in_quotes = false;
                }
            } else {
                current.text += c;
            }
        } else if (c == '"' && current.text.empty() && !current.quoted) {
            in_quotes = true;
            current.quoted = true;
        } else if (c == delimiter) {
            fields.push_back(std::move(current));
            current = Field();
        } else {
            current.text += c;
        }
    }
    fields.push_back(std::move(current));
    return !in_quotes;
}

// Follows, byte by byte, whether a quoted field is open in the record being read: the same rules
// as SplitRecord (a quote is structural only at the start of a field, or doubled inside a quoted
// one), but incremental, so a record spanning many lines is scanned once, not once per line.
class QuoteScanner {
  public:
    explicit QuoteScanner(char delimiter) : delimiter_(delimiter) {}
    void Reset() { state_ = State::FieldStart; }
    void Step(char c) {
        switch (state_) {
        case State::FieldStart:
            state_ = c == '"' ? State::InQuotes
                              : (c == delimiter_ ? State::FieldStart : State::Unquoted);
            break;
        case State::Unquoted:
            state_ = c == delimiter_ ? State::FieldStart : State::Unquoted;
            break;
        case State::InQuotes:
            state_ = c == '"' ? State::QuoteSeen : State::InQuotes;
            break;
        case State::QuoteSeen: // `""` is an escaped quote; anything else closed the field
            state_ = c == '"' ? State::InQuotes
                              : (c == delimiter_ ? State::FieldStart : State::Unquoted);
            break;
        }
    }
    void Feed(const char* data, size_t n) {
        for (size_t i = 0; i < n; i++) {
            Step(data[i]);
        }
    }
    // A newline here ends the record iff this is false.
    bool InQuotes() const { return state_ == State::InQuotes; }

  private:
    enum class State { FieldStart, Unquoted, InQuotes, QuoteSeen };
    char delimiter_;
    State state_ = State::FieldStart;
};

template <class T> bool ParseNumber(const std::string& s, T& out) {
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end;
}

void StoreField(Vector& vec, idx_t row, const Field& f, LogicalType type, idx_t line,
                const std::string& column) {
    if (f.text.empty() && (!f.quoted || type.id() != TypeId::Varchar)) {
        vec.Validity().SetInvalid(row); // unquoted empty (or quoted empty non-text) = NULL
        return;
    }
    try {
        switch (type.id()) {
        case TypeId::Varchar:
            vec.FlatData<string_t>()[row] = vec.AddString(f.text);
            return;
        case TypeId::Integer: {
            int32_t v;
            if (ParseNumber(f.text, v)) {
                vec.FlatData<int32_t>()[row] = v;
                return;
            }
            break;
        }
        case TypeId::BigInt: {
            int64_t v;
            if (ParseNumber(f.text, v)) {
                vec.FlatData<int64_t>()[row] = v;
                return;
            }
            break;
        }
        case TypeId::Double: {
            double v;
            if (ParseNumber(f.text, v)) {
                vec.FlatData<double>()[row] = v;
                return;
            }
            break;
        }
        case TypeId::Date: {
            if (auto d = Date::FromString(f.text)) {
                vec.FlatData<int32_t>()[row] = d->days;
                return;
            }
            break;
        }
        case TypeId::Boolean:
            break;
        }
        // Slow path: the general conversion (handles whitespace, 'true'/'t', '1.5' -> 2, inf, ...)
        const Value v = CastValue(Value::Varchar(f.text), type);
        vec.SetValue(row, v);
    } catch (const Error& e) {
        std::string msg = e.what();
        const std::string prefix = std::string(ErrorCodeName(e.code())) + ": ";
        if (msg.rfind(prefix, 0) == 0)
            msg = msg.substr(prefix.size());
        RowError(line, column, msg);
    }
}

// Stores one complete, split record as row `row` of `chunk`. Shared by the serial and the parallel
// loader, so both accept the same input and fail with the same message.
void StoreRecord(std::vector<Field>& fields, const std::vector<ColumnDefinition>& schema,
                 const std::vector<LogicalType>& types, idx_t line, DataChunk& chunk, idx_t row) {
    if (fields.size() == schema.size() + 1 && fields.back().text.empty() && !fields.back().quoted) {
        fields.pop_back(); // dbgen-style trailing delimiter
    }
    if (fields.size() != schema.size()) {
        RowError(line, "",
                 "expected " + std::to_string(schema.size()) + " fields but found " +
                     std::to_string(fields.size()));
    }
    for (idx_t c = 0; c < schema.size(); c++) {
        StoreField(chunk.column(c), row, fields[c], types[c], line, schema[c].name);
    }
}

} // namespace

idx_t LoadCsv(Table& target, std::istream& input, const CsvOptions& options) {
    const auto& schema = target.schema();
    std::vector<LogicalType> types;
    for (const auto& c : schema)
        types.push_back(c.type);

    auto staging = std::make_unique<Table>("csv_staging", schema, target.row_group_size());
    DataChunk chunk;
    chunk.Initialize(types);
    idx_t rows_in_chunk = 0, total = 0;
    auto flush = [&] {
        if (rows_in_chunk == 0)
            return;
        chunk.SetCardinality(rows_in_chunk);
        staging->Append(chunk);
        chunk.Reset();
        rows_in_chunk = 0;
    };

    std::vector<Field> fields;
    std::string record, line;
    idx_t line_no = 0, record_line = 0;
    bool skipped_header = !options.header;
    QuoteScanner scanner(options.delimiter);
    while (std::getline(input, line)) {
        line_no++;
        // `line` still carries a CRLF line ending's '\r': it must survive inside a quoted field
        // that spans lines, and is stripped only when the record turns out to be complete.
        if (record.empty()) {
            if (line.empty() || line == "\r")
                continue; // blank line
            record_line = line_no;
            record = line;
            scanner.Reset();
        } else {
            record += "\n" + line;
            scanner.Step('\n');
        }
        scanner.Feed(line.data(), line.size());
        if (scanner.InQuotes()) {
            continue; // the quoted field continues on the next line
        }
        const std::string* text = &record;
        std::string stripped;
        if (record.back() == '\r') {
            stripped.assign(record, 0, record.size() - 1);
            text = &stripped;
        }
        [[maybe_unused]] const bool complete = SplitRecord(*text, options.delimiter, fields);
        CDB_ASSERT(complete); // the scanner and SplitRecord must agree on where a record ends
        record.clear();
        if (!skipped_header) {
            skipped_header = true;
            continue;
        }
        StoreRecord(fields, schema, types, record_line, chunk, rows_in_chunk);
        rows_in_chunk++;
        total++;
        if (rows_in_chunk == kVectorSize)
            flush();
    }
    if (!record.empty()) {
        RowError(record_line, "", "unterminated quoted field at end of input");
    }
    flush();
    target.Merge(std::move(staging));
    return total;
}

// ---------------------------------------------------------------------------------- parallel

#ifdef CDB_CSV_HAS_MMAP
namespace {

// Where one record lives in the mapped file. `line` is the physical line it starts on, for errors.
struct Record {
    uint64_t begin;
    uint32_t length; // without the line terminator
    uint32_t line;
};

// Drops the pages fully inside [begin, end) of the mapping from this process's resident set once
// they have been read (they stay in the page cache and fault back in if touched again), so a big
// file does not all sit in memory at once. Pages shared with a neighbouring range are kept.
void ReleasePages(const char* data, uint64_t begin, uint64_t end) {
    static const auto page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    const uint64_t first = (begin + page - 1) / page * page;
    const uint64_t last = end / page * page;
    if (last > first) {
        madvise(const_cast<char*>(data) + first, last - first, MADV_DONTNEED);
    }
}

bool IsBlank(const char* text, uint32_t length) {
    return length == 0 || (length == 1 && text[0] == '\r');
}

// Record boundaries when the data may contain quotes: a state machine that mirrors SplitRecord (a
// quote is structural only at the start of a field, or doubled inside a quoted one), so a newline
// inside quotes does not end a record. Serial, about a byte per cycle or two. A record still open
// at the end of the data (an unterminated quote) is returned as is; parsing it reports the error
// in file order.
std::vector<Record> FindRecordsQuoted(const char* data, size_t size, char delimiter) {
    std::vector<Record> records;
    QuoteScanner scanner(delimiter);
    size_t start = 0;
    uint32_t line = 1, start_line = 1;
    const auto end_record = [&](size_t end) {
        if (!IsBlank(data + start, static_cast<uint32_t>(end - start))) {
            records.push_back({start, static_cast<uint32_t>(end - start), start_line});
        }
    };
    for (size_t i = 0; i < size; i++) {
        const char c = data[i];
        if (c == '\n') {
            line++;
            if (!scanner.InQuotes()) {
                end_record(i);
                start = i + 1;
                start_line = line;
                scanner.Reset();
                continue;
            }
        }
        scanner.Step(c);
    }
    if (start < size) {
        end_record(size);
    }
    return records;
}

// Record boundaries when the data contains no quote at all: every newline ends a record, so the
// file is cut into byte chunks and each chunk finds its own newlines (two parallel passes: count,
// then write). Returns nullopt if a quote is found (use FindRecordsQuoted), or if a record is too
// long for the 32-bit length.
std::optional<std::vector<Record>>
FindRecordsUnquoted(const char* data, size_t size, size_t chunk_bytes, TaskScheduler& scheduler) {
    const size_t chunks = (size + chunk_bytes - 1) / chunk_bytes;
    std::vector<size_t> newlines(chunks, 0);
    std::vector<int64_t> last_newline(chunks, -1); // position of the chunk's last '\n', or -1
    std::atomic<bool> has_quote{false};
    scheduler.ParallelFor(chunks, [&](size_t c) {
        const char* begin = data + c * chunk_bytes;
        const char* end = data + std::min(size, (c + 1) * chunk_bytes);
        if (std::memchr(begin, '"', static_cast<size_t>(end - begin)) != nullptr) {
            has_quote.store(true, std::memory_order_relaxed);
            return;
        }
        size_t count = 0;
        for (const char* p = begin;
             (p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)))) !=
             nullptr;
             p++) {
            count++;
            last_newline[c] = p - data;
        }
        newlines[c] = count;
        ReleasePages(data, static_cast<uint64_t>(begin - data), static_cast<uint64_t>(end - data));
    });
    if (has_quote.load()) {
        return std::nullopt;
    }
    // Line k (0-based) ends at the k-th newline. First line of each chunk, and the newline before
    // it.
    std::vector<size_t> first_line(chunks, 0);
    std::vector<int64_t> newline_before(chunks, -1); // last '\n' before the chunk, or -1
    size_t total = 0;
    int64_t last = -1;
    for (size_t c = 0; c < chunks; c++) {
        first_line[c] = total;
        newline_before[c] = last;
        total += newlines[c];
        if (last_newline[c] >= 0) {
            last = last_newline[c];
        }
    }
    if (total >= UINT32_MAX) {
        return std::nullopt;
    }
    std::vector<Record> lines(total + 1);
    std::atomic<bool> too_long{false};
    scheduler.ParallelFor(chunks, [&](size_t c) {
        const char* begin = data + c * chunk_bytes;
        const char* end = data + std::min(size, (c + 1) * chunk_bytes);
        int64_t previous = newline_before[c];
        size_t k = first_line[c];
        for (const char* p = begin;
             (p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)))) !=
             nullptr;
             p++) {
            const int64_t at = p - data;
            const uint64_t length = static_cast<uint64_t>(at - (previous + 1));
            if (length >= UINT32_MAX) {
                too_long.store(true, std::memory_order_relaxed);
            }
            lines[k] = {static_cast<uint64_t>(previous + 1), static_cast<uint32_t>(length),
                        static_cast<uint32_t>(k + 1)};
            previous = at;
            k++;
        }
        ReleasePages(data, static_cast<uint64_t>(begin - data), static_cast<uint64_t>(end - data));
    });
    if (too_long.load()) {
        return std::nullopt;
    }
    // The text after the last newline, if any, is a final record without a terminator.
    const uint64_t tail_begin = static_cast<uint64_t>(last + 1);
    if (tail_begin < size) {
        if (size - tail_begin >= UINT32_MAX) {
            return std::nullopt;
        }
        lines[total] = {tail_begin, static_cast<uint32_t>(size - tail_begin),
                        static_cast<uint32_t>(total + 1)};
    } else {
        lines.pop_back();
    }
    // Blank lines are not records.
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [&](const Record& r) { return IsBlank(data + r.begin, r.length); }),
                lines.end());
    return lines;
}

std::atomic<uint64_t> g_parallel_loads{0};

struct Mapping {
    const char* data = nullptr;
    size_t size = 0;
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() {
        if (data != nullptr) {
            munmap(const_cast<char*>(data), size);
        }
    }
    bool Open(const std::string& path) {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            close(fd);
            return false;
        }
        void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            return false;
        }
        madvise(p, static_cast<size_t>(st.st_size), MADV_SEQUENTIAL);
        data = static_cast<const char*>(p);
        size = static_cast<size_t>(st.st_size);
        return true;
    }
};

// Loads the file on all of the scheduler's threads. Returns nullopt (nothing done) if the file is
// not suitable: too small, not a regular file, or too big for the 32-bit record fields.
std::optional<idx_t> LoadCsvParallel(Table& target, const std::string& path,
                                     const CsvOptions& options, TaskScheduler& scheduler) {
    Mapping file;
    if (!file.Open(path) || file.size < options.parallel_min_bytes ||
        file.size >= (size_t{4} << 30)) {
        return std::nullopt;
    }
    const char* data = file.data;

    const size_t chunk_bytes = options.parallel_chunk_bytes != 0
                                   ? options.parallel_chunk_bytes
                                   : std::clamp<size_t>(file.size / (scheduler.threads() * 8),
                                                        size_t{1} << 20, size_t{64} << 20);
    std::optional<std::vector<Record>> found =
        FindRecordsUnquoted(data, file.size, chunk_bytes, scheduler);
    std::vector<Record> records =
        found ? std::move(*found) : FindRecordsQuoted(data, file.size, options.delimiter);
    if (options.header && !records.empty()) {
        // The serial loader skips the header only once it is a complete record: one that ends
        // inside an open quote (it can only be the last, so only if it is the only record) is an
        // error.
        std::vector<Field> header_fields;
        const Record& h = records.front();
        if (!SplitRecord(std::string(data + h.begin, h.length), options.delimiter, header_fields)) {
            RowError(h.line, "", "unterminated quoted field at end of input");
        }
        records.erase(records.begin());
    }

    const auto& schema = target.schema();
    std::vector<LogicalType> types;
    for (const auto& c : schema) {
        types.push_back(c.type);
    }
    const idx_t group_rows = target.row_group_size();
    const idx_t rows = records.size();
    auto staging = std::make_unique<Table>("csv_staging", schema, group_rows);

    // One task per full row group, plus one for the rows left over (they become the open tail).
    const size_t full_groups = rows / group_rows;
    const size_t tasks = full_groups + (rows % group_rows != 0 ? 1 : 0);
    std::vector<std::shared_ptr<const RowGroup>> sealed(full_groups);
    std::vector<DataChunk> tail_chunks;
    std::vector<std::exception_ptr> errors(tasks);
    std::atomic<size_t> first_failed{SIZE_MAX};

    scheduler.ParallelFor(tasks, [&](size_t g) {
        if (g > first_failed.load(std::memory_order_relaxed)) {
            return; // an earlier row group already failed; its error is the one to report
        }
        try {
            const idx_t first = g * group_rows;
            const idx_t last = std::min<idx_t>(rows, first + group_rows);
            std::optional<RowGroupBuilder> builder;
            if (g < full_groups) {
                builder.emplace(schema, group_rows);
            }
            std::vector<Field> fields;
            std::string record;
            DataChunk chunk;
            chunk.Initialize(types);
            for (idx_t at = first; at < last; at += kVectorSize) {
                const idx_t n = std::min<idx_t>(kVectorSize, last - at);
                chunk.Reset();
                for (idx_t i = 0; i < n; i++) {
                    const Record& r = records[at + i];
                    record.assign(data + r.begin, r.length);
                    const std::string* text = &record;
                    std::string stripped;
                    if (record.back() == '\r') {
                        stripped.assign(record, 0, record.size() - 1);
                        text = &stripped;
                    }
                    if (!SplitRecord(*text, options.delimiter, fields)) {
                        RowError(r.line, "", "unterminated quoted field at end of input");
                    }
                    StoreRecord(fields, schema, types, r.line, chunk, i);
                }
                chunk.SetCardinality(n);
                staging->ValidateChunk(chunk); // NOT NULL, per vector as the serial loader does
                if (builder) {
                    builder->Append(chunk, 0, n);
                } else {
                    DataChunk copy;
                    copy.Initialize(types, n);
                    for (idx_t c = 0; c < types.size(); c++) {
                        VectorOps::Copy(chunk.column(c), copy.column(c), nullptr, n);
                    }
                    copy.SetCardinality(n);
                    tail_chunks.push_back(std::move(copy)); // only the one tail task writes this
                }
            }
            if (builder) {
                sealed[g] = builder->Seal();
            }
            if (last > first) { // the group's text has been parsed: let go of its pages
                ReleasePages(data, records[first].begin,
                             records[last - 1].begin + records[last - 1].length);
            }
        } catch (...) {
            errors[g] = std::current_exception();
            size_t seen = first_failed.load();
            while (g < seen && !first_failed.compare_exchange_weak(seen, g)) {
            }
        }
    });
    if (const size_t failed = first_failed.load(); failed != SIZE_MAX) {
        std::rethrow_exception(errors[failed]);
    }

    staging->AppendRowGroups(std::move(sealed));
    for (const DataChunk& chunk : tail_chunks) {
        staging->Append(chunk);
    }
    target.Merge(std::move(staging));
    g_parallel_loads++;
    return rows;
}

} // namespace
#endif // CDB_CSV_HAS_MMAP

size_t DefaultCsvParallelMinBytes() noexcept {
    static const size_t bytes = [] {
        const char* env = std::getenv("CDB_CSV_PARALLEL_MIN_BYTES");
        return env != nullptr ? static_cast<size_t>(std::strtoull(env, nullptr, 10))
                              : size_t{8} << 20;
    }();
    return bytes;
}

uint64_t CsvParallelLoadCount() noexcept {
#ifdef CDB_CSV_HAS_MMAP
    return g_parallel_loads.load();
#else
    return 0;
#endif
}

idx_t LoadCsvFile(Table& target, const std::string& path, const CsvOptions& options,
                  TaskScheduler* scheduler) {
#ifdef CDB_CSV_HAS_MMAP
    if (scheduler != nullptr && scheduler->threads() > 1) {
        if (const std::optional<idx_t> rows = LoadCsvParallel(target, path, options, *scheduler)) {
            return *rows;
        }
    }
#else
    (void)scheduler;
#endif
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::Io, "cannot open file '" + path + "'");
    }
    return LoadCsv(target, in, options);
}

} // namespace cdb
