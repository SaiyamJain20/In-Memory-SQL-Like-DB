#include "common/string_ops.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <map>
#include <utility>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

std::string Sub(std::string_view s, long long start, std::optional<long long> len = std::nullopt) {
    return std::string(SubstringView(s, start, len));
}

} // namespace

TEST(Utf8, CharLengthOfValidSequences) {
    EXPECT_EQ(Utf8CharLength("a", 0), 1U);
    EXPECT_EQ(Utf8CharLength("\xC3\xA4", 0), 2U);         // a-umlaut
    EXPECT_EQ(Utf8CharLength("\xE2\x82\xAC", 0), 3U);     // euro sign
    EXPECT_EQ(Utf8CharLength("\xF0\x9F\x98\x80", 0), 4U); // emoji
    EXPECT_EQ(Utf8CharLength("x\xC3\xA4", 1), 2U);
}

TEST(Utf8, InvalidBytesAreCharactersOfTheirOwn) {
    EXPECT_EQ(Utf8CharLength("\x80", 0), 1U); // stray continuation
    EXPECT_EQ(Utf8CharLength("\xFF"
                             "aa",
                             0),
              1U); // invalid lead: must not swallow what follows
    EXPECT_EQ(Utf8CharLength("\xF8\x80\x80\x80\x80", 0), 1U);
    EXPECT_EQ(Utf8CharLength("\xE2\x82", 0), 1U); // truncated at the end
    EXPECT_EQ(Utf8CharLength("\xC3"
                             "a",
                             0),
              1U); // lead followed by a non-continuation
    EXPECT_EQ(Utf8CharLength("\xE2\x82"
                             "a",
                             0),
              1U);
    EXPECT_EQ(Utf8CharLength("\xC3", 0), 1U);
}

TEST(Utf8, LengthCountsCharacters) {
    EXPECT_EQ(Utf8Length(""), 0U);
    EXPECT_EQ(Utf8Length("hello"), 5U);
    EXPECT_EQ(Utf8Length("h\xC3\xA4llo"), 5U);
    EXPECT_EQ(Utf8Length("\xE2\x82\xAC\xF0\x9F\x98\x80"), 2U);
    EXPECT_EQ(Utf8Length("\xFF"
                         "aa"),
              3U);
    EXPECT_EQ(Utf8Length("\xE2\x82"), 2U); // truncated: two stray bytes
    EXPECT_EQ(Utf8Length(std::string("\0\0", 2)), 2U);
}

TEST(Utf8, ATruncatedSequenceNeverLooksPastTheEndOfTheView) {
    // The view ends in the middle of a valid character, but the bytes that would complete it exist
    // in memory right after the view. They must not be read: the cut-off bytes are characters of
    // their own. (A NUL-terminated literal would hide a bug here: the byte after the end is 0, not
    // a continuation byte.)
    const char buf[] = "\xF0\x9F\x98\x80\xE2\x82\xAC\xC3\xA4";
    EXPECT_EQ(Utf8CharLength(std::string_view(buf, 3), 0),
              1U); // 4-byte character cut after 3 bytes
    EXPECT_EQ(Utf8Length(std::string_view(buf, 3)), 3U);
    EXPECT_EQ(Utf8CharLength(std::string_view(buf, 4), 0), 4U);     // complete
    EXPECT_EQ(Utf8CharLength(std::string_view(buf + 4, 2), 0), 1U); // 3-byte cut after 2
    EXPECT_EQ(Utf8Length(std::string_view(buf + 4, 2)), 2U);
    EXPECT_EQ(Utf8CharLength(std::string_view(buf + 7, 1), 0), 1U); // 2-byte cut after 1
    EXPECT_EQ(Utf8Length(std::string_view(buf + 7, 1)), 1U);
    EXPECT_EQ(Utf8Length(std::string_view(buf + 7, 2)), 1U); // ... and complete
    // the substring and LIKE built on it agree
    EXPECT_EQ(SubstringView(std::string_view(buf + 4, 2), 1, 1), std::string_view(buf + 4, 1));
    EXPECT_TRUE(LikeMatch(std::string_view(buf + 4, 2), "__"));
    EXPECT_FALSE(LikeMatch(std::string_view(buf + 4, 2), "_"));
}

TEST(Utf8, LengthMatchesCharacterWalkOnRandomBytes) {
    Rng rng(11);
    for (int iter = 0; iter < 2000; iter++) {
        std::string s = test::RandomString(rng);
        size_t chars = 0;
        for (size_t i = 0; i < s.size(); i += Utf8CharLength(s, i)) {
            ASSERT_GE(Utf8CharLength(s, i), 1U);
            ASSERT_LE(Utf8CharLength(s, i), s.size() - i)
                << "a character must not run past the end";
            chars++;
        }
        EXPECT_EQ(Utf8Length(s), chars);
    }
}

TEST(Substring, BasicSemantics) {
    EXPECT_EQ(Sub("hello", 2, 3), "ell");
    EXPECT_EQ(Sub("hello", 1), "hello");
    EXPECT_EQ(Sub("hello", 2), "ello");
    EXPECT_EQ(Sub("hello", 5, 1), "o");
    EXPECT_EQ(Sub("hello", 6, 1), "");
    EXPECT_EQ(Sub("hello", 100, 1), "");
    EXPECT_EQ(Sub("hello", 1, 0), "");
    EXPECT_EQ(Sub("", 1, 5), "");
    EXPECT_EQ(Sub("hello", 1, 100), "hello");
}

TEST(Substring, ZeroAndNegativeStart) {
    EXPECT_EQ(Sub("hello", 0, 2), "h"); // start 0 is one before the first character
    EXPECT_EQ(Sub("hello", 0), "hello");
    EXPECT_EQ(Sub("hello", -1, 1), "o"); // negative start counts from the end
    EXPECT_EQ(Sub("hello", -3, 2), "ll");
    EXPECT_EQ(Sub("hello", -5), "hello");
    EXPECT_EQ(Sub("hello", -9, 5), "h"); // window is positions -4..0: only the first character
    EXPECT_EQ(Sub("hello", -9, 3), "");
}

TEST(Substring, NegativeLengthTakesCharactersBeforeStart) {
    EXPECT_EQ(Sub("hello", 3, -2), "he");
    EXPECT_EQ(Sub("hello", 1, -2), "");
    EXPECT_EQ(Sub("hello", 6, -2), "lo");
}

TEST(Substring, CountsCharactersNotBytes) {
    const std::string s = "\xC3\xA4"
                          "b\xE2\x82\xAC"
                          "d"; // a-umlaut b euro d
    EXPECT_EQ(Sub(s, 1, 1), "\xC3\xA4");
    EXPECT_EQ(Sub(s, 2, 2), "b\xE2\x82\xAC");
    EXPECT_EQ(Sub(s, 3), "\xE2\x82\xAC"
                         "d");
    EXPECT_EQ(Sub(s, -1), "d");
    EXPECT_EQ(Sub(s, -2, 1), "\xE2\x82\xAC");
}

TEST(Substring, ResultIsAViewIntoTheInputAndExtremeArgumentsAreSafe) {
    const std::string s = "abcdef";
    const std::string_view v = SubstringView(s, 2, 3);
    EXPECT_EQ(v, "bcd");
    EXPECT_EQ(v.data(), s.data() + 1) << "must alias the input, not copy";
    constexpr long long kMax = std::numeric_limits<long long>::max() / 4;
    EXPECT_EQ(Sub(s, kMax, 5), "");
    EXPECT_EQ(Sub(s, -kMax, 5), "");
    EXPECT_EQ(Sub(s, 1, kMax), "abcdef");
    EXPECT_EQ(Sub(s, 3, -kMax), "ab"); // everything before position 3
}

TEST(Substring, InvalidUtf8NeverEscapesTheInput) {
    Rng rng(5);
    for (int iter = 0; iter < 3000; iter++) {
        const std::string s = test::RandomString(rng);
        const long long start = static_cast<long long>(RandBelow(rng, 30)) - 12;
        std::optional<long long> len;
        if (Chance(rng, 0.7)) {
            len = static_cast<long long>(RandBelow(rng, 30)) - 8;
        }
        const std::string_view v = SubstringView(s, start, len);
        if (!v.empty()) { // (an empty result may be a null view)
            ASSERT_GE(v.data(), s.data());
            ASSERT_LE(v.data() + v.size(), s.data() + s.size());
        }
    }
}

TEST(Like, Basics) {
    EXPECT_TRUE(LikeMatch("", ""));
    EXPECT_TRUE(LikeMatch("", "%"));
    EXPECT_TRUE(LikeMatch("abc", "%"));
    EXPECT_TRUE(LikeMatch("abc", "abc"));
    EXPECT_FALSE(LikeMatch("abc", "ab"));
    EXPECT_FALSE(LikeMatch("ab", "abc"));
    EXPECT_TRUE(LikeMatch("abc", "a%"));
    EXPECT_TRUE(LikeMatch("abc", "%c"));
    EXPECT_TRUE(LikeMatch("abc", "%b%"));
    EXPECT_TRUE(LikeMatch("abc", "a_c"));
    EXPECT_FALSE(LikeMatch("abc", "a_"));
    EXPECT_TRUE(LikeMatch("abc", "___"));
    EXPECT_FALSE(LikeMatch("abc", "____"));
    EXPECT_FALSE(LikeMatch("", "_"));
    EXPECT_TRUE(LikeMatch("abc", "%%%"));
    EXPECT_TRUE(LikeMatch("aXbXc", "a%b%c"));
    EXPECT_FALSE(LikeMatch("aXbXc", "a%b%d"));
    EXPECT_FALSE(LikeMatch("ABC", "abc")); // case sensitive
    EXPECT_TRUE(LikeMatch("100%", "100%"));
    EXPECT_TRUE(LikeMatch("a\\b", "a\\b")); // backslash is an ordinary character
}

TEST(Like, UnderscoreMatchesOneCharacterNotOneByte) {
    EXPECT_TRUE(LikeMatch("\xC3\xA4", "_"));
    EXPECT_FALSE(LikeMatch("\xC3\xA4", "__"));
    EXPECT_TRUE(LikeMatch("a\xE2\x82\xAC"
                          "b",
                          "a_b"));
    EXPECT_TRUE(LikeMatch("\xF0\x9F\x98\x80", "_"));
    EXPECT_TRUE(LikeMatch("x\xC3\xA4y", "%\xC3\xA4%"));
    EXPECT_FALSE(LikeMatch("x\xC3\xA4y", "%\xA4%x")); // a continuation byte alone is not a boundary
}

TEST(Like, InvalidLeadByteDoesNotSwallowFollowingText) {
    // Regression: "\xFF" used to be a 3-byte character, so '%' skipped the whole string and the
    // suffix 'a' could not match - while byte-wise search found it.
    EXPECT_TRUE(LikeMatch("\xFF"
                          "aa",
                          "%a"));
    EXPECT_TRUE(LikeMatch("\xFF"
                          "aa",
                          "%a%"));
    EXPECT_TRUE(LikeMatch("\xFF"
                          "aa",
                          "_aa"));
    EXPECT_TRUE(LikeMatch("\xC3"
                          "a",
                          "_a"));
}

TEST(Like, NoExponentialBacktracking) {
    const std::string text(5000, 'a');
    EXPECT_FALSE(LikeMatch(text, "%a%a%a%a%a%a%a%a%a%a%a%a%a%b"));
    EXPECT_TRUE(LikeMatch(text, "%a%a%a%a%a%a%a%a%a%a%a%a%a%a"));
}

namespace {

// An independent, exhaustive (memoised) matcher with the same semantics: a literal byte consumes
// one byte, '_' and '%' consume characters as segmented at their current position.
class ReferenceLike {
  public:
    ReferenceLike(std::string_view text, std::string_view pattern) : text_(text), pat_(pattern) {}
    bool Run() { return Match(0, 0); }

  private:
    bool Match(size_t t, size_t p) {
        const auto key = std::make_pair(t, p);
        if (auto it = memo_.find(key); it != memo_.end()) {
            return it->second;
        }
        bool r;
        if (p == pat_.size()) {
            r = t == text_.size();
        } else if (pat_[p] == '%') {
            r = Match(t, p + 1) || (t < text_.size() && Match(t + Utf8CharLength(text_, t), p));
        } else if (t == text_.size()) {
            r = false;
        } else if (pat_[p] == '_') {
            r = Match(t + Utf8CharLength(text_, t), p + 1);
        } else {
            r = pat_[p] == text_[t] && Match(t + 1, p + 1);
        }
        return memo_[key] = r;
    }
    std::string_view text_, pat_;
    std::map<std::pair<size_t, size_t>, bool> memo_;
};

// Strings over a tiny alphabet that mixes ASCII, lead bytes, continuation bytes and invalid
// bytes, so character boundaries are as ambiguous as they can get.
std::string TinyString(Rng& rng, size_t max_len, bool with_wildcards) {
    static const char kPlain[] = {'a', 'b', '\xC3', '\xA4', '\xE2', '\x82', '\xAC', '\xFF', '\x80'};
    std::string s;
    const size_t len = RandBelow(rng, max_len + 1);
    for (size_t i = 0; i < len; i++) {
        if (with_wildcards && Chance(rng, 0.3)) {
            s += Chance(rng, 0.5) ? '%' : '_';
        } else {
            s += kPlain[RandBelow(rng, sizeof(kPlain))];
        }
    }
    return s;
}

} // namespace

TEST(Like, MatchesExhaustiveReferenceOnAdversarialBytes) {
    Rng rng(2024);
    for (int iter = 0; iter < 20000; iter++) {
        const std::string text = TinyString(rng, 9, false);
        const std::string pattern = TinyString(rng, 7, true);
        ASSERT_EQ(LikeMatch(text, pattern), ReferenceLike(text, pattern).Run())
            << "text bytes: " << text.size() << " pattern: " << pattern;
    }
}

TEST(Like, LiteralOnlyPatternsAreByteEquality) {
    Rng rng(3);
    for (int iter = 0; iter < 5000; iter++) {
        const std::string a = TinyString(rng, 6, false), b = TinyString(rng, 6, false);
        ASSERT_EQ(LikeMatch(a, b), a == b);
    }
}

} // namespace cdb
