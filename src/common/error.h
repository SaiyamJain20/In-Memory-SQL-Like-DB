#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace cdb {

// Category of a query-level error. See docs/adr/0002-error-handling.md.
enum class ErrorCode : uint8_t {
    Internal,       // engine bug or violated invariant that was recoverable
    Syntax,         // lexer / parser
    Binder,         // unknown table/column, ambiguous reference, bad function arguments
    Catalog,        // object already exists / does not exist
    Type,           // type mismatch, invalid cast
    Execution,      // runtime failure: overflow, division by zero, out of memory
    Io,             // file system failure
    Corruption,     // a database file failed validation (checksum, structure, format version)
    NotImplemented, // valid SQL that this engine does not support (yet)
};

const char* ErrorCodeName(ErrorCode code) noexcept;

// Thrown for expected, user-facing failures. Never thrown per-row from inner loops.
class Error : public std::runtime_error {
  public:
    Error(ErrorCode code, const std::string& message);
    // `position` is a byte offset into the SQL text, used to point at the offending token.
    Error(ErrorCode code, const std::string& message, size_t position);

    ErrorCode code() const noexcept { return code_; }
    std::optional<size_t> position() const noexcept { return position_; }

  private:
    ErrorCode code_;
    std::optional<size_t> position_;
};

} // namespace cdb
