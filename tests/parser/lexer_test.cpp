#include "common/error.h"
#include "parser/token.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

std::vector<Token> Lex(std::string_view sql) {
    return Tokenize(sql);
}

// "type:text" per token, e.g. "KW:SELECT", "ID:a", "INT:1", "STR:x", "SYM:(", ending with "END".
std::vector<std::string> Describe(std::string_view sql) {
    std::vector<std::string> out;
    for (const Token& t : Lex(sql)) {
        const char* tag = "";
        switch (t.type) {
        case TokenType::End:
            tag = "END";
            break;
        case TokenType::Identifier:
            tag = "ID";
            break;
        case TokenType::QuotedIdentifier:
            tag = "QID";
            break;
        case TokenType::Keyword:
            tag = "KW";
            break;
        case TokenType::Integer:
            tag = "INT";
            break;
        case TokenType::Decimal:
            tag = "DEC";
            break;
        case TokenType::String:
            tag = "STR";
            break;
        case TokenType::Symbol:
            tag = "SYM";
            break;
        }
        out.push_back(t.type == TokenType::End ? "END" : std::string(tag) + ":" + t.text);
    }
    return out;
}

using V = std::vector<std::string>;

size_t ErrorPos(std::string_view sql) {
    try {
        Lex(sql);
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Syntax) << sql;
        EXPECT_TRUE(e.position().has_value()) << sql;
        return e.position().value_or(~size_t{0});
    }
    ADD_FAILURE() << "expected a lexer error for: " << sql;
    return 0;
}

} // namespace

TEST(Lexer, EmptyAndWhitespaceOnly) {
    EXPECT_EQ(Describe(""), V{"END"});
    EXPECT_EQ(Describe("  \t\n\r "), V{"END"});
    EXPECT_EQ(Lex("   ").back().pos, 3u);
}

TEST(Lexer, KeywordsAreCaseInsensitiveAndUpperCased) {
    EXPECT_EQ(Describe("select SeLeCt SELECT from Where null"),
              (V{"KW:SELECT", "KW:SELECT", "KW:SELECT", "KW:FROM", "KW:WHERE", "KW:NULL", "END"}));
}

TEST(Lexer, ContextualWordsAreIdentifiers) {
    // these are only special in certain syntactic positions, so they must stay usable as names
    EXPECT_EQ(Describe("date interval extract substring first last year analyze header"),
              (V{"ID:date", "ID:interval", "ID:extract", "ID:substring", "ID:first", "ID:last",
                 "ID:year", "ID:analyze", "ID:header", "END"}));
}

TEST(Lexer, IdentifiersAreLowerCasedQuotedOnesAreVerbatim) {
    EXPECT_EQ(Describe("Foo BAR_1 _x a$b"), (V{"ID:foo", "ID:bar_1", "ID:_x", "ID:a$b", "END"}));
    EXPECT_EQ(Describe("\"Foo Bar\" \"select\" \"a\"\"b\""),
              (V{"QID:Foo Bar", "QID:select", "QID:a\"b", "END"})); // quoted keyword is a name
}

TEST(Lexer, NonAsciiBytesFormIdentifiers) {
    EXPECT_EQ(Describe("na\xc3\xafve caf\xc3\xa9"),
              (V{"ID:na\xc3\xafve", "ID:caf\xc3\xa9", "END"}));
}

TEST(Lexer, IntegersAndDecimals) {
    EXPECT_EQ(Describe("0 7 123456789012"), (V{"INT:0", "INT:7", "INT:123456789012", "END"}));
    EXPECT_EQ(Describe("1.5 .5 1. 0.05 1e10 1.5E-3 2e+4 3.e2"),
              (V{"DEC:1.5", "DEC:.5", "DEC:1.", "DEC:0.05", "DEC:1e10", "DEC:1.5E-3", "DEC:2e+4",
                 "DEC:3.e2", "END"}));
}

TEST(Lexer, MalformedNumbers) {
    EXPECT_EQ(ErrorPos("1e"), 0u);
    EXPECT_EQ(ErrorPos("1e+"), 0u);
    EXPECT_EQ(ErrorPos("SELECT 12abc"), 7u);
    EXPECT_EQ(ErrorPos("1.5x"), 0u);
    EXPECT_EQ(ErrorPos("SELECT 1_000"), 7u);
}

TEST(Lexer, DotIsASymbolUnlessFollowedByADigit) {
    EXPECT_EQ(Describe("t.c"), (V{"ID:t", "SYM:.", "ID:c", "END"}));
    EXPECT_EQ(Describe("t.5"), (V{"ID:t", "DEC:.5", "END"})); // lexically a decimal
    EXPECT_EQ(Describe("a . b"), (V{"ID:a", "SYM:.", "ID:b", "END"}));
}

TEST(Lexer, StringLiteralsWithEscapes) {
    EXPECT_EQ(Describe("'' 'abc' 'it''s' 'a''''b'"),
              (V{"STR:", "STR:abc", "STR:it's", "STR:a''b", "END"}));
    EXPECT_EQ(Describe("'line1\nline2'"), (V{"STR:line1\nline2", "END"}));
    EXPECT_EQ(Describe("'--not a comment' '/*nor this*/'"),
              (V{"STR:--not a comment", "STR:/*nor this*/", "END"}));
}

TEST(Lexer, UnterminatedLiteralsPointAtTheOpeningQuote) {
    EXPECT_EQ(ErrorPos("SELECT 'abc"), 7u);
    EXPECT_EQ(ErrorPos("'a''"), 0u);
    EXPECT_EQ(ErrorPos("SELECT \"abc"), 7u);
    EXPECT_EQ(ErrorPos("\"\""), 0u); // zero-length quoted identifier
}

TEST(Lexer, Comments) {
    EXPECT_EQ(Describe("a -- trailing\nb"), (V{"ID:a", "ID:b", "END"}));
    EXPECT_EQ(Describe("a -- no newline at eof"), (V{"ID:a", "END"}));
    EXPECT_EQ(Describe("a /* block */ b"), (V{"ID:a", "ID:b", "END"}));
    EXPECT_EQ(Describe("a /* multi\nline */ b"), (V{"ID:a", "ID:b", "END"}));
    EXPECT_EQ(Describe("a /**/ b"), (V{"ID:a", "ID:b", "END"}));
    EXPECT_EQ(Describe("1-2"), (V{"INT:1", "SYM:-", "INT:2", "END"})); // not a comment
    EXPECT_EQ(Describe("1 - -2"), (V{"INT:1", "SYM:-", "SYM:-", "INT:2", "END"}));
    EXPECT_EQ(Describe("4/2"), (V{"INT:4", "SYM:/", "INT:2", "END"}));
    EXPECT_EQ(ErrorPos("a /* never closed"), 2u);
    EXPECT_EQ(ErrorPos("/*"), 0u);
    // block comments do not nest: the first */ ends the comment, the rest lexes as symbols
    EXPECT_EQ(Describe("/* a /* b */ c */"), (V{"ID:c", "SYM:*", "SYM:/", "END"}));
}

TEST(Lexer, Symbols) {
    EXPECT_EQ(
        Describe("( ) , ; . * + - / % = < > <= >= <> || ::"),
        (V{"SYM:(", "SYM:)", "SYM:,", "SYM:;", "SYM:.", "SYM:*", "SYM:+", "SYM:-", "SYM:/", "SYM:%",
           "SYM:=", "SYM:<", "SYM:>", "SYM:<=", "SYM:>=", "SYM:<>", "SYM:||", "SYM:::", "END"}));
    EXPECT_EQ(Describe("a!=b"), (V{"ID:a", "SYM:<>", "ID:b", "END"})); // normalised
    EXPECT_EQ(Describe("a<=b>=c"), (V{"ID:a", "SYM:<=", "ID:b", "SYM:>=", "ID:c", "END"}));
    EXPECT_EQ(Describe("a<>b"), (V{"ID:a", "SYM:<>", "ID:b", "END"}));
    EXPECT_EQ(Describe("a< >b"), (V{"ID:a", "SYM:<", "SYM:>", "ID:b", "END"}));
}

TEST(Lexer, UnexpectedCharacters) {
    EXPECT_EQ(ErrorPos("SELECT @x"), 7u);
    EXPECT_EQ(ErrorPos("a # b"), 2u);
    EXPECT_EQ(ErrorPos("a | b"), 2u); // a single | is not an operator
    EXPECT_EQ(ErrorPos("a ! b"), 2u);
    EXPECT_EQ(ErrorPos("a : b"), 2u);
    EXPECT_EQ(ErrorPos(std::string("ab\0cd", 5)), 2u); // embedded NUL
    EXPECT_EQ(ErrorPos("\\"), 0u);
}

TEST(Lexer, PositionsAndLengthsCoverTheSource) {
    const std::string sql = "SELECT  a1,\n  'x y' ,\"Q\"<=12.5";
    auto toks = Lex(sql);
    for (const Token& t : toks) {
        if (t.type == TokenType::End)
            continue;
        ASSERT_LE(t.pos + t.length, sql.size());
        const std::string raw = sql.substr(t.pos, t.length);
        switch (t.type) {
        case TokenType::Keyword:
        case TokenType::Identifier:
            EXPECT_EQ(raw.size(), t.text.size());
            break;
        case TokenType::String:
            EXPECT_EQ(raw, "'" + t.text + "'");
            break;
        case TokenType::QuotedIdentifier:
            EXPECT_EQ(raw, "\"" + t.text + "\"");
            break;
        case TokenType::Integer:
        case TokenType::Decimal:
            EXPECT_EQ(raw, t.text);
            break;
        case TokenType::Symbol:
            EXPECT_EQ(raw, t.text);
            break;
        case TokenType::End:
            break;
        }
    }
    EXPECT_EQ(toks[0].pos, 0u);
    EXPECT_EQ(toks[1].pos, 8u); // "a1" after two spaces
    EXPECT_EQ(toks.back().type, TokenType::End);
    EXPECT_EQ(toks.back().pos, sql.size());
    // positions strictly increase and spans never overlap
    for (size_t i = 1; i < toks.size(); i++) {
        EXPECT_GE(toks[i].pos, toks[i - 1].pos + toks[i - 1].length);
    }
}

TEST(Lexer, KeywordNames) {
    EXPECT_STREQ(KeywordName(Keyword::SELECT), "SELECT");
    EXPECT_STREQ(KeywordName(Keyword::NULL_), "NULL_");
    EXPECT_STREQ(KeywordName(Keyword::None), "");
}

// Arbitrary bytes must never crash the lexer: it either yields tokens ending in End, or throws a
// positioned Syntax error. Run under ASan/UBSan.
TEST(Lexer, RandomBytesNeverCrash) {
    test::Rng rng(2024);
    static const char kAlphabet[] = "abcXYZ_019 \t\n'\"-/*.+<>=!|:;,()$@#\\eE\xff\x80";
    size_t ok = 0, rejected = 0;
    for (int i = 0; i < 30000; i++) {
        std::string s(test::RandBelow(rng, 40), ' ');
        for (char& c : s)
            c = kAlphabet[test::RandBelow(rng, sizeof(kAlphabet) - 1)];
        try {
            auto toks = Tokenize(s);
            ASSERT_FALSE(toks.empty());
            ASSERT_EQ(toks.back().type, TokenType::End);
            for (const Token& t : toks)
                ASSERT_LE(t.pos + t.length, s.size());
            ok++;
        } catch (const Error& e) {
            ASSERT_EQ(e.code(), ErrorCode::Syntax);
            ASSERT_TRUE(e.position().has_value());
            ASSERT_LE(*e.position(), s.size());
            rejected++;
        }
    }
    EXPECT_GT(ok, 1000u); // both outcomes must actually occur
    EXPECT_GT(rejected, 1000u);
}

} // namespace cdb
