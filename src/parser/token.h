#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cdb {

enum class TokenType : uint8_t {
    End,
    Identifier,       // unquoted; text is lower-cased
    QuotedIdentifier, // "Like This"; text is verbatim (with "" unescaped)
    Keyword,          // reserved word; text is upper-cased
    Integer,          // digits only
    Decimal,          // has a '.' or an exponent
    String,           // 'text'; text is the unescaped contents
    Symbol,           // ( ) , ; . * + - / % = < > <= >= <> || ::
};

// Reserved words. Words that are only special in certain positions (DATE, INTERVAL, EXTRACT,
// SUBSTRING, FIRST, LAST, ANALYZE, HEADER, DELIMITER, units like DAY ...) are lexed as plain
// identifiers and recognised contextually by the parser, so they remain usable as column names.
// clang-format off
#define CDB_KEYWORD_LIST(X)                                                                        \
    X(ALL)                                                                                         \
    X(AND)                                                                                         \
    X(AS)                                                                                          \
    X(ASC)                                                                                         \
    X(BETWEEN)                                                                                     \
    X(BY)                                                                                          \
    X(CASE)                                                                                        \
    X(CAST)                                                                                        \
    X(COPY)                                                                                        \
    X(CREATE) X(CROSS) X(DESC) X(DISTINCT) X(DROP) X(ELSE) X(END) X(EXCEPT) X(EXISTS) X(EXPLAIN)   \
        X(FALSE) X(FROM) X(FULL) X(GROUP) X(HAVING) X(IF) X(IN) X(INNER) X(INSERT) X(INTERSECT)    \
            X(INTO) X(IS) X(JOIN) X(LEFT) X(LIKE) X(LIMIT) X(NOT) X(NULL_) X(OFFSET) X(ON) X(OR)   \
                X(ORDER) X(OUTER) X(PRIMARY) X(RIGHT) X(SELECT) X(TABLE) X(THEN) X(TRUE) X(UNION)  \
                    X(UNIQUE) X(USING) X(VALUES) X(WHEN) X(WHERE) X(WITH)
// clang-format on

enum class Keyword : uint8_t {
    None,
#define CDB_KEYWORD_ENUM(name) name,
    CDB_KEYWORD_LIST(CDB_KEYWORD_ENUM)
#undef CDB_KEYWORD_ENUM
};

// Display name of a keyword ("NULL" for NULL_), or "" for Keyword::None.
const char* KeywordName(Keyword keyword) noexcept;

struct Token {
    TokenType type = TokenType::End;
    Keyword keyword = Keyword::None;
    std::string text;
    size_t pos = 0;    // byte offset of the first character in the SQL text
    size_t length = 0; // bytes covered in the SQL text

    bool IsKeyword(Keyword k) const noexcept { return type == TokenType::Keyword && keyword == k; }
    bool IsSymbol(std::string_view s) const noexcept {
        return type == TokenType::Symbol && text == s;
    }
};

// Splits SQL text into tokens (the last one is always TokenType::End). Throws Error(Syntax) with
// a byte position for: unterminated strings / quoted identifiers / block comments, empty quoted
// identifiers, malformed numbers, and unexpected characters (including NUL bytes).
// `--` line comments and `/* */` block comments (non-nested) are skipped. `!=` is normalised to
// `<>`.
std::vector<Token> Tokenize(std::string_view sql);

} // namespace cdb
