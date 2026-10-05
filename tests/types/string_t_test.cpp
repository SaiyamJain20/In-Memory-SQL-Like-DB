#include "types/string_t.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cdb {

namespace {

// Owns the bytes for out-of-line strings so tests can build string_t from std::string.
struct Holder {
    std::string text;
    string_t s;
    explicit Holder(std::string t) : text(std::move(t)), s(string_t::FromView(text)) {}
    Holder(const Holder&) = delete;
};

int Sign(int x) {
    return x < 0 ? -1 : (x > 0 ? 1 : 0);
}

} // namespace

TEST(StringT, LayoutIsSixteenBytes) {
    static_assert(sizeof(string_t) == 16);
    static_assert(std::is_trivially_copyable_v<string_t>);
}

TEST(StringT, DefaultIsEmptyInlined) {
    string_t s;
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(s.IsInlined());
    EXPECT_EQ(s.view(), "");
}

TEST(StringT, InlineBoundaryIsTwelveBytes) {
    for (uint32_t len = 0; len <= 40; len++) {
        const std::string text(len, 'x');
        string_t s = string_t::FromView(text);
        EXPECT_EQ(s.size(), len);
        EXPECT_EQ(s.IsInlined(), len <= 12) << len;
        EXPECT_EQ(s.view(), text) << len;
        if (!s.IsInlined()) {
            EXPECT_EQ(s.data(), text.data()); // referenced, not copied
        }
    }
}

TEST(StringT, PrefixHoldsFirstFourBytesZeroPadded) {
    const std::string abcdef = "abcdef";
    string_t s = string_t::FromView(abcdef);
    char expect[4] = {'a', 'b', 'c', 'd'};
    uint32_t p = s.Prefix();
    EXPECT_EQ(std::memcmp(&p, expect, 4), 0);

    string_t two = string_t::FromView("hi");
    uint32_t p2 = two.Prefix();
    char expect2[4] = {'h', 'i', 0, 0};
    EXPECT_EQ(std::memcmp(&p2, expect2, 4), 0);

    const std::string long_text(30, 'q');
    string_t long_s = string_t::FromView(long_text);
    uint32_t p3 = long_s.Prefix();
    EXPECT_EQ(std::memcmp(&p3, "qqqq", 4), 0);
}

TEST(StringT, EmbeddedNulsAndHighBytesRoundTrip) {
    const std::string text("a\0b\xff\x80z", 6);
    string_t s = string_t::FromView(text);
    EXPECT_EQ(s.size(), 6u);
    EXPECT_EQ(s.view(), std::string_view(text));
}

TEST(StringT, EqualityBasics) {
    Holder a("hello"), b("hello"), c("hellp"), d("hell");
    EXPECT_TRUE(a.s == b.s);
    EXPECT_FALSE(a.s == c.s);
    EXPECT_FALSE(a.s == d.s);
    EXPECT_TRUE(a.s != c.s);

    Holder l1("this string is definitely longer than twelve");
    Holder l2("this string is definitely longer than twelve");
    Holder l3("this string is definitely longer than twelvE");
    Holder l4("this string is definitely longer than twelve!");
    EXPECT_TRUE(l1.s == l2.s);
    EXPECT_FALSE(l1.s == l3.s); // differs only in the last byte
    EXPECT_FALSE(l1.s == l4.s); // differs only in length
    EXPECT_NE(l1.s.data(), l2.s.data());
}

TEST(StringT, EqualityDistinguishesTrailingNulFromShorterString) {
    Holder a(std::string("ab", 2)), b(std::string("ab\0", 3)), c(std::string("ab\0\0", 4));
    EXPECT_FALSE(a.s == b.s);
    EXPECT_FALSE(b.s == c.s);
    EXPECT_FALSE(a.s == c.s);
}

TEST(StringT, CompareIsUnsignedBytewise) {
    Holder lo("\x01"), hi("\xff"), mid("\x7f");
    EXPECT_LT(string_t::Compare(lo.s, mid.s), 0);
    EXPECT_LT(string_t::Compare(mid.s, hi.s), 0); // 0xff > 0x7f: not a signed-char compare
    EXPECT_GT(string_t::Compare(hi.s, lo.s), 0);
}

TEST(StringT, CompareShorterPrefixSortsFirst) {
    Holder a("abc"), b("abcd"), c("abcdefghijklmnop"), d("abcdefghijklmnopq");
    EXPECT_LT(string_t::Compare(a.s, b.s), 0);
    EXPECT_LT(string_t::Compare(b.s, c.s), 0);
    EXPECT_LT(string_t::Compare(c.s, d.s), 0);
    EXPECT_GT(string_t::Compare(d.s, a.s), 0);
    EXPECT_EQ(string_t::Compare(c.s, c.s), 0);
}

TEST(StringT, CompareHandlesEmbeddedNuls) {
    Holder a(std::string("a", 1)), b(std::string("a\0", 2)), c(std::string("a\0\0\0\0\0", 6)),
        d(std::string("a\0b", 3));
    EXPECT_LT(string_t::Compare(a.s, b.s), 0);
    EXPECT_LT(string_t::Compare(b.s, c.s), 0);
    EXPECT_LT(string_t::Compare(b.s, d.s), 0);
    EXPECT_GT(string_t::Compare(d.s, c.s), 0);
}

TEST(StringT, EmptyStringsCompare) {
    Holder e1(""), e2(""), a("a");
    EXPECT_EQ(string_t::Compare(e1.s, e2.s), 0);
    EXPECT_TRUE(e1.s == e2.s);
    EXPECT_LT(string_t::Compare(e1.s, a.s), 0);
    EXPECT_GT(string_t::Compare(a.s, e1.s), 0);
}

TEST(StringT, OperatorsAgreeWithCompare) {
    Holder a("apple"), b("banana");
    EXPECT_TRUE(a.s < b.s);
    EXPECT_TRUE(a.s <= b.s);
    EXPECT_TRUE(b.s > a.s);
    EXPECT_TRUE(b.s >= a.s);
    EXPECT_TRUE(a.s <= a.s);
    EXPECT_TRUE(a.s >= a.s);
    EXPECT_FALSE(a.s < a.s);
}

// Property test: against std::string_view as the oracle, over strings engineered to collide on
// prefixes, straddle the inline boundary and contain NULs / high bytes.
TEST(StringT, RandomizedComparisonMatchesStringView) {
    test::Rng rng(12345);
    std::vector<std::unique_ptr<Holder>> pool;
    for (int i = 0; i < 400; i++) {
        pool.push_back(std::make_unique<Holder>(test::RandomString(rng)));
    }
    size_t equal_pairs = 0;
    for (const auto& a : pool) {
        for (const auto& b : pool) {
            const int expect = Sign(std::string_view(a->text).compare(std::string_view(b->text)));
            ASSERT_EQ(Sign(string_t::Compare(a->s, b->s)), expect)
                << "a.size=" << a->text.size() << " b.size=" << b->text.size();
            ASSERT_EQ(a->s == b->s, expect == 0);
            ASSERT_EQ(a->s < b->s, expect < 0);
            equal_pairs += expect == 0;
        }
    }
    // The generator must actually produce ties (incl. identical strings), or the test is weak.
    EXPECT_GT(equal_pairs, pool.size());
}

} // namespace cdb
