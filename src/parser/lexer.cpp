#include "common/error.h"
#include "parser/token.h"

#include <cctype>
#include <unordered_map>

namespace cdb {

const char* KeywordName(Keyword keyword) noexcept {
    switch (keyword) {
    case Keyword::None:
        return "";
#define CDB_KEYWORD_NAME(name)                                                                     \
    case Keyword::name:                                                                            \
        return #name;
        CDB_KEYWORD_LIST(CDB_KEYWORD_NAME)
#undef CDB_KEYWORD_NAME
    }
    return "";
}

namespace {

const std::unordered_map<std::string, Keyword>& KeywordTable() {
    static const std::unordered_map<std::string, Keyword> table = [] {
        std::unordered_map<std::string, Keyword> t;
#define CDB_KEYWORD_ENTRY(name) t.emplace(#name, Keyword::name);
        CDB_KEYWORD_LIST(CDB_KEYWORD_ENTRY)
#undef CDB_KEYWORD_ENTRY
        t.erase("NULL_");
        t.emplace("NULL", Keyword::NULL_);
        return t;
    }();
    return table;
}

bool IsIdentStart(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool IsIdentChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

bool IsDigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

} // namespace

std::vector<Token> Tokenize(std::string_view sql) {
    std::vector<Token> tokens;
    const size_t n = sql.size();
    size_t i = 0;

    auto fail = [&](const std::string& message, size_t pos) -> void {
        throw Error(ErrorCode::Syntax, message, pos);
    };
    auto push = [&](TokenType type, std::string text, size_t pos, size_t end,
                    Keyword kw = Keyword::None) {
        Token t;
        t.type = type;
        t.keyword = kw;
        t.text = std::move(text);
        t.pos = pos;
        t.length = end - pos;
        tokens.push_back(std::move(t));
    };

    while (i < n) {
        const auto c = static_cast<unsigned char>(sql[i]);

        if (c == 0) {
            fail("unexpected NUL byte in SQL text", i);
        }
        if (std::isspace(c)) {
            i++;
            continue;
        }
        // comments
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n')
                i++;
            continue;
        }
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            const size_t start = i;
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/'))
                i++;
            if (i + 1 >= n)
                fail("unterminated /* comment", start);
            i += 2;
            continue;
        }
        // identifiers and keywords
        if (IsIdentStart(c)) {
            const size_t start = i;
            while (i < n && IsIdentChar(static_cast<unsigned char>(sql[i])))
                i++;
            std::string word(sql.substr(start, i - start));
            std::string upper = word, lower = word;
            for (char& ch : upper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            for (char& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            auto it = KeywordTable().find(upper);
            if (it != KeywordTable().end()) {
                push(TokenType::Keyword, std::move(upper), start, i, it->second);
            } else {
                push(TokenType::Identifier, std::move(lower), start, i);
            }
            continue;
        }
        // quoted identifiers
        if (c == '"') {
            const size_t start = i++;
            std::string text;
            bool closed = false;
            while (i < n) {
                if (sql[i] == '"') {
                    if (i + 1 < n && sql[i + 1] == '"') {
                        text += '"';
                        i += 2;
                        continue;
                    }
                    closed = true;
                    i++;
                    break;
                }
                text += sql[i++];
            }
            if (!closed)
                fail("unterminated quoted identifier", start);
            if (text.empty())
                fail("zero-length quoted identifier", start);
            push(TokenType::QuotedIdentifier, std::move(text), start, i);
            continue;
        }
        // string literals
        if (c == '\'') {
            const size_t start = i++;
            std::string text;
            bool closed = false;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        text += '\'';
                        i += 2;
                        continue;
                    }
                    closed = true;
                    i++;
                    break;
                }
                text += sql[i++];
            }
            if (!closed)
                fail("unterminated string literal", start);
            push(TokenType::String, std::move(text), start, i);
            continue;
        }
        // numbers: 123, 1.5, .5, 1., 1e10, 1.5E-3
        if (IsDigit(c) ||
            (c == '.' && i + 1 < n && IsDigit(static_cast<unsigned char>(sql[i + 1])))) {
            const size_t start = i;
            bool decimal = false;
            while (i < n && IsDigit(static_cast<unsigned char>(sql[i])))
                i++;
            if (i < n && sql[i] == '.') {
                decimal = true;
                i++;
                while (i < n && IsDigit(static_cast<unsigned char>(sql[i])))
                    i++;
            }
            if (i < n && (sql[i] == 'e' || sql[i] == 'E')) {
                size_t j = i + 1;
                if (j < n && (sql[j] == '+' || sql[j] == '-'))
                    j++;
                if (j < n && IsDigit(static_cast<unsigned char>(sql[j]))) {
                    decimal = true;
                    while (j < n && IsDigit(static_cast<unsigned char>(sql[j])))
                        j++;
                    i = j;
                } else {
                    fail("malformed numeric literal: exponent has no digits", start);
                }
            }
            if (i < n && IsIdentStart(static_cast<unsigned char>(sql[i]))) {
                fail("trailing characters after numeric literal", start);
            }
            push(decimal ? TokenType::Decimal : TokenType::Integer,
                 std::string(sql.substr(start, i - start)), start, i);
            continue;
        }
        // symbols
        const size_t start = i;
        auto two = [&](char a, char b) {
            return c == static_cast<unsigned char>(a) && i + 1 < n && sql[i + 1] == b;
        };
        if (two('<', '=') || two('>', '=') || two('<', '>') || two('|', '|') || two(':', ':')) {
            push(TokenType::Symbol, std::string(sql.substr(i, 2)), start, i + 2);
            i += 2;
            continue;
        }
        if (two('!', '=')) {
            push(TokenType::Symbol, "<>", start, i + 2);
            i += 2;
            continue;
        }
        switch (c) {
        case '(':
        case ')':
        case ',':
        case ';':
        case '.':
        case '*':
        case '+':
        case '-':
        case '/':
        case '%':
        case '=':
        case '<':
        case '>':
            push(TokenType::Symbol, std::string(1, static_cast<char>(c)), start, i + 1);
            i++;
            continue;
        default:
            break;
        }
        std::string shown = std::isprint(c) ? std::string(1, static_cast<char>(c))
                                            : "\\x" + std::string(1, "0123456789abcdef"[c >> 4]) +
                                                  std::string(1, "0123456789abcdef"[c & 15]);
        fail("unexpected character '" + shown + "'", i);
    }
    push(TokenType::End, "", n, n);
    return tokens;
}

} // namespace cdb
