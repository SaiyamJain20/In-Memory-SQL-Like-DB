#include "io/csv_reader.h"

#include "common/error.h"
#include "types/cast.h"

#include <charconv>
#include <fstream>

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
    while (std::getline(input, line)) {
        line_no++;
        // `line` still carries a CRLF line ending's '\r': it must survive inside a quoted field
        // that spans lines, and is stripped only when the record turns out to be complete.
        if (record.empty()) {
            if (line.empty() || line == "\r")
                continue; // blank line
            record_line = line_no;
            record = line;
        } else {
            record += "\n" + line;
        }
        const std::string* text = &record;
        std::string stripped;
        if (record.back() == '\r') {
            stripped.assign(record, 0, record.size() - 1);
            text = &stripped;
        }
        if (!SplitRecord(*text, options.delimiter, fields))
            continue; // the quoted field continues on the next line
        record.clear();
        if (!skipped_header) {
            skipped_header = true;
            continue;
        }
        if (fields.size() == schema.size() + 1 && fields.back().text.empty() &&
            !fields.back().quoted) {
            fields.pop_back(); // dbgen-style trailing delimiter
        }
        if (fields.size() != schema.size()) {
            RowError(record_line, "",
                     "expected " + std::to_string(schema.size()) + " fields but found " +
                         std::to_string(fields.size()));
        }
        for (idx_t c = 0; c < schema.size(); c++) {
            StoreField(chunk.column(c), rows_in_chunk, fields[c], types[c], record_line,
                       schema[c].name);
        }
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

idx_t LoadCsvFile(Table& target, const std::string& path, const CsvOptions& options) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::Io, "cannot open file '" + path + "'");
    }
    return LoadCsv(target, in, options);
}

} // namespace cdb
