#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace cdb {

// Byte length of the character starting at s[pos] (pos < s.size()). A lead byte only starts a
// multi-byte character if the right number of continuation bytes (10xxxxxx) follow; any other
// byte (stray continuation, invalid lead, truncated sequence) is a character of its own. So the
// text is segmented the same way for every input, valid UTF-8 or not, and an ASCII byte is
// always the start of a character - which lets LIKE use byte-wise search for ASCII-led literals.
size_t Utf8CharLength(std::string_view s, size_t pos) noexcept;

// Number of UTF-8 characters in `s`.
size_t Utf8Length(std::string_view s) noexcept;

// SQL substring over UTF-8 characters, DuckDB semantics: `start` is 1-based; start 0 behaves as
// one before the first character; a negative start counts from the end; a negative `length` takes
// that many characters *before* the start. The result is a view into `s` (a substring of UTF-8
// text is always contiguous bytes).
std::string_view SubstringView(std::string_view s, long long start,
                               std::optional<long long> length) noexcept;

// SQL LIKE: '%' matches any sequence, '_' any single (UTF-8) character, there is no escape
// character. Case-sensitive. Linear-ish: no exponential backtracking.
bool LikeMatch(std::string_view text, std::string_view pattern) noexcept;

} // namespace cdb
