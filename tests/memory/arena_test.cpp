#include "memory/arena.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace cdb {

TEST(Arena, ZeroSizeAllocationReturnsNonNull) {
    Arena a;
    EXPECT_NE(a.Allocate(0), nullptr);
    EXPECT_NE(a.Allocate(0, 64), nullptr);
}

TEST(Arena, RespectsAlignment) {
    Arena a(1024);
    for (size_t align :
         {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{32}, size_t{64}}) {
        for (size_t size : {size_t{1}, size_t{3}, size_t{17}, size_t{100}, size_t{600}}) {
            void* p = a.Allocate(size, align);
            ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % align, 0u) << "align=" << align;
        }
    }
}

TEST(Arena, AllocationsDoNotOverlapAndSurviveGrowth) {
    // Many allocations crossing many block boundaries; pointers must stay valid and contents
    // intact. Under ASan, any overrun or use-after-free is reported.
    Arena a(4096);
    test::Rng rng(1);
    struct Rec {
        uint8_t* p;
        size_t n;
        uint8_t tag;
    };
    std::vector<Rec> recs;
    for (int i = 0; i < 5000; i++) {
        const size_t n = 1 + test::RandBelow(rng, i % 50 == 0 ? 9000 : 200);
        auto* p = static_cast<uint8_t*>(a.Allocate(n, 1));
        const auto tag = static_cast<uint8_t>(i);
        std::memset(p, tag, n);
        recs.push_back({p, n, tag});
    }
    for (const Rec& r : recs) {
        for (size_t i = 0; i < r.n; i++) {
            ASSERT_EQ(r.p[i], r.tag);
        }
    }
    EXPECT_GT(a.BlockCount(), 1u);
}

TEST(Arena, LargeAllocationGetsDedicatedBlockAndKeepsActiveBlock) {
    Arena a(1024);
    auto* small1 = static_cast<uint8_t*>(a.Allocate(10, 1));
    const size_t blocks_before = a.BlockCount();
    auto* big = static_cast<uint8_t*>(a.Allocate(100000, 1));
    std::memset(big, 0x5A, 100000);
    EXPECT_EQ(a.BlockCount(), blocks_before + 1);
    // Small allocations continue in the original block, right after the first one.
    auto* small2 = static_cast<uint8_t*>(a.Allocate(10, 1));
    EXPECT_EQ(small2, small1 + 10);
    EXPECT_EQ(a.BlockCount(), blocks_before + 1);
}

TEST(Arena, AccountingTracksUsedAndReserved) {
    Arena a(1024);
    EXPECT_EQ(a.BytesUsed(), 0u);
    a.Allocate(100, 1);
    a.Allocate(50, 1);
    EXPECT_EQ(a.BytesUsed(), 150u);
    EXPECT_GE(a.BytesReserved(), a.BytesUsed());
}

TEST(Arena, ResetKeepsOneBlockAndReusesIt) {
    Arena a(1024);
    auto* first = static_cast<uint8_t*>(a.Allocate(100, 1));
    for (int i = 0; i < 20; i++)
        a.Allocate(900, 1);
    a.Allocate(50000, 1);
    EXPECT_GT(a.BlockCount(), 2u);

    a.Reset();
    EXPECT_EQ(a.BytesUsed(), 0u);
    EXPECT_EQ(a.BlockCount(), 1u);
    auto* again = static_cast<uint8_t*>(a.Allocate(100, 1));
    EXPECT_EQ(again, first); // the retained block is reused from its start
    std::memset(again, 1, 100);
}

TEST(Arena, ResetOnEmptyArenaIsSafe) {
    Arena a;
    a.Reset();
    a.Reset();
    EXPECT_EQ(a.BlockCount(), 0u);
}

TEST(Arena, MoveTransfersOwnership) {
    Arena a(2048);
    auto* p = static_cast<uint8_t*>(a.Allocate(64, 1));
    std::memset(p, 7, 64);
    Arena b(std::move(a));
    EXPECT_EQ(b.BytesUsed(), 64u);
    EXPECT_EQ(p[10], 7);          // memory still owned (by b)
    EXPECT_EQ(a.BytesUsed(), 0u); // NOLINT: moved-from state is specified: empty
    Arena c;
    c = std::move(b);
    EXPECT_EQ(c.BytesUsed(), 64u);
    EXPECT_EQ(p[63], 7);
}

} // namespace cdb
