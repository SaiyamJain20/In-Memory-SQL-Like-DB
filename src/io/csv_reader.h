#pragma once

#include "execution/task_scheduler.h"
#include "storage/table.h"

#include <functional>
#include <istream>
#include <memory>
#include <string>

namespace cdb {

// 8 MiB, or CDB_CSV_PARALLEL_MIN_BYTES from the environment (the parallel test presets lower it so
// that every COPY in the suite takes the parallel path).
size_t DefaultCsvParallelMinBytes() noexcept;

struct CsvOptions {
    char delimiter = ',';
    bool header = false; // skip the first record
    // LoadCsvFile() with a scheduler loads files of at least this many bytes in parallel (smaller
    // ones are not worth the setup).
    size_t parallel_min_bytes = DefaultCsvParallelMinBytes();
    // Parallel loading finds record boundaries in byte chunks of this size (0 = chosen from the
    // file size and the thread count). Tests use tiny chunks to put boundaries everywhere.
    size_t parallel_chunk_bytes = 0;
    // What to do with the fully built staging table once the whole input is valid. Empty: merge it
    // into the target (Table::Merge). A persistent database sets this to log the rows before they
    // are merged.
    std::function<void(std::unique_ptr<Table> staging)> commit = {};
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
//
// LoadCsvFile() with a scheduler of more than one thread and a big enough file maps the file,
// finds the record boundaries, and parses whole row groups on all threads (each parses, seals and
// compresses its own group); the groups are then published in file order. The resulting table, its
// row groups and every error message (including the line number) are exactly what the serial
// loader produces. Streams and small files take the serial path.
idx_t LoadCsv(Table& target, std::istream& input, const CsvOptions& options);
idx_t LoadCsvFile(Table& target, const std::string& path, const CsvOptions& options,
                  TaskScheduler* scheduler = nullptr);

// How many loads have used the parallel path so far in this process (so tests can tell it from the
// fallback to the serial one).
uint64_t CsvParallelLoadCount() noexcept;

} // namespace cdb
