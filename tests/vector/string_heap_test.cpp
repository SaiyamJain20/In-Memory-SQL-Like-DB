#include "vector/string_heap.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cdb {

TEST(StringHeap, ShortStringsAreInlinedAndUseNoHeapMemory) {
    StringHeap h;
    for (size_t len = 0; len <= 12; len++) {
        const std::string text(len, 'z');
        string_t s = h.Add(text);
        EXPECT_TRUE(s.IsInlined()) << len;
        EXPECT_EQ(s.view(), text);
    }
    EXPECT_EQ(h.BytesUsed(), 0u);
}

TEST(StringHeap, LongStringsAreCopiedIntoTheHeap) {
    StringHeap h;
    std::string text = "this is a rather long string, certainly over twelve bytes";
    string_t s = h.Add(text);
    EXPECT_FALSE(s.IsInlined());
    EXPECT_EQ(h.BytesUsed(), text.size());
    // The heap owns its own copy: mutating or destroying the source must not matter.
    const std::string original = text;
    text.assign(text.size(), '#');
    text.clear();
    text.shrink_to_fit();
    EXPECT_EQ(s.view(), original);
}

TEST(StringHeap, StringsStayValidAcrossMassiveGrowth) {
    StringHeap h;
    test::Rng rng(5);
    std::vector<std::string> expected;
    std::vector<string_t> stored;
    for (int i = 0; i < 20000; i++) {
        expected.push_back(test::RandomString(rng));
        stored.push_back(h.Add(expected.back()));
    }
    for (size_t i = 0; i < expected.size(); i++) {
        ASSERT_EQ(stored[i].view(), std::string_view(expected[i])) << i;
    }
}

TEST(StringHeap, VeryLargeString) {
    StringHeap h;
    std::string big(5 * 1024 * 1024, 'q');
    big[big.size() / 2] = 'X';
    string_t s = h.Add(big);
    EXPECT_EQ(s.size(), big.size());
    EXPECT_EQ(s.view(), std::string_view(big));
}

TEST(StringHeap, ResetReleasesMemoryAndAllowsReuse) {
    StringHeap h;
    h.Add(std::string(1000, 'a'));
    EXPECT_GT(h.BytesUsed(), 0u);
    h.Reset();
    EXPECT_EQ(h.BytesUsed(), 0u);
    string_t s = h.Add(std::string(50, 'b'));
    EXPECT_EQ(s.view(), std::string(50, 'b'));
}

TEST(StringHeap, SealedHeapKeepsExistingStringsAndAllowsInlineAdds) {
    StringHeap h;
    string_t long_s = h.Add(std::string(40, 'q'));
    EXPECT_FALSE(h.sealed());
    h.Seal();
    EXPECT_TRUE(h.sealed());
    EXPECT_EQ(long_s.view(), std::string(40, 'q'));
    // inlined strings need no heap memory, so they remain addable
    EXPECT_EQ(h.Add("tiny").view(), "tiny");
}

TEST(StringHeapDeathTest, SealedHeapRejectsOutOfLineAddsAndReset) {
    StringHeap h;
    h.Seal();
    EXPECT_DEATH(h.Add(std::string(40, 'x')), "CDB_CHECK");
    EXPECT_DEATH(h.Reset(), "CDB_CHECK");
}

} // namespace cdb
