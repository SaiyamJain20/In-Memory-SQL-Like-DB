#pragma once

#include "storage/table.h"

#include <istream>
#include <string>

namespace cdb {

struct CsvOptions {
    char delimiter = ',';
    bool header = false; // skip the first record
};

// Loads CSV text into `target`, atomically: rows are parsed into a private staging table and
// merged in only if the whole input is valid, so an error leaves `target` untouched and readers
// never see a partial load. Returns the number of rows loaded.
//
// Format: RFC 4180-style quoting ("" escapes a quote; quoted fields may contain delimiters and
// newlines); LF or CRLF line ends; blank lines are skipped. An unquoted empty field is NULL; a
// quoted empty field ("") is the empty string (NULL for non-VARCHAR columns). One extra empty
// trailing field is tolerated (dbgen-style `a|b|c|`).
//
// Errors: Error(Execution) naming the line and column for the wrong field count or a value that
// does not convert to its column type; Error(Io) if the file cannot be opened.
idx_t LoadCsv(Table& target, std::istream& input, const CsvOptions& options);
idx_t LoadCsvFile(Table& target, const std::string& path, const CsvOptions& options);

} // namespace cdb
