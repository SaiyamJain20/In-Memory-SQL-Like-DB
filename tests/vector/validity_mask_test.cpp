#include "vector/validity_mask.h"
#include <cstring>

#include "test_util.h"

#include <gtest/gtest.h>

#include <vector>

namespace cdb {

TEST(ValidityMask, DefaultIsAllValidWithoutStorage) {
    ValidityMask m(100);
    EXPECT_TRUE(m.AllValid());
    EXPECT_EQ(m.Words(), nullptr);
    for (idx_t i = 0; i < 100; i++)
        EXPECT_TRUE(m.IsValid(i));
    EXPECT_EQ(m.CountValid(100), 100u);
}

TEST(ValidityMask, SetValidOnUnallocatedMaskDoesNotAllocate) {
    ValidityMask m(100);
    m.SetValid(5);
    EXPECT_TRUE(m.AllValid());
}

TEST(ValidityMask, SetInvalidAllocatesLazilyAndAffectsOnlyThatRow) {
    ValidityMask m(200);
    m.SetInvalid(70);
    EXPECT_FALSE(m.AllValid());
    for (idx_t i = 0; i < 200; i++)
        EXPECT_EQ(m.IsValid(i), i != 70) << i;
    EXPECT_EQ(m.CountValid(200), 199u);
    m.SetValid(70);
    EXPECT_TRUE(m.IsValid(70));
    EXPECT_EQ(m.CountValid(200), 200u);
}

TEST(ValidityMask, WordBoundaries) {
    for (idx_t row : {idx_t{0}, idx_t{1}, idx_t{62}, idx_t{63}, idx_t{64}, idx_t{65}, idx_t{127},
                      idx_t{128}, idx_t{2047}}) {
        ValidityMask m(2048);
        m.SetInvalid(row);
        for (idx_t i = 0; i < 2048; i++)
            ASSERT_EQ(m.IsValid(i), i != row) << "row=" << row;
    }
}

TEST(ValidityMask, OddCapacities) {
    for (idx_t cap : {idx_t{1}, idx_t{2}, idx_t{63}, idx_t{64}, idx_t{65}, idx_t{127}, idx_t{129},
                      idx_t{2047}}) {
        ValidityMask m(cap);
        m.SetInvalid(cap - 1);
        EXPECT_FALSE(m.IsValid(cap - 1));
        if (cap > 1) {
            EXPECT_TRUE(m.IsValid(0));
        }
        EXPECT_EQ(m.CountValid(cap), cap - 1) << cap;
    }
}

TEST(ValidityMask, CountValidRespectsCountArgument) {
    ValidityMask m(256);
    m.SetInvalid(3);
    m.SetInvalid(64);
    m.SetInvalid(200);
    EXPECT_EQ(m.CountValid(0), 0u);
    EXPECT_EQ(m.CountValid(3), 3u);
    EXPECT_EQ(m.CountValid(4), 3u);
    EXPECT_EQ(m.CountValid(64), 63u);
    EXPECT_EQ(m.CountValid(65), 63u);
    EXPECT_EQ(m.CountValid(128), 126u);
    EXPECT_EQ(m.CountValid(256), 253u);
}

TEST(ValidityMask, SetAllInvalidOnlyTouchesFirstCountRows) {
    for (idx_t count : {idx_t{0}, idx_t{1}, idx_t{63}, idx_t{64}, idx_t{65}, idx_t{100}, idx_t{128},
                        idx_t{300}}) {
        ValidityMask m(300);
        m.SetAllInvalid(count);
        for (idx_t i = 0; i < 300; i++)
            ASSERT_EQ(m.IsValid(i), i >= count) << count;
        EXPECT_EQ(m.CountValid(300), 300 - count);
    }
}

TEST(ValidityMask, SetRangeValidOnUnallocatedMaskIsANoOp) {
    ValidityMask m(100);
    m.SetRangeValid(10, 50);
    EXPECT_TRUE(m.AllValid());
}

TEST(ValidityMask, SetRangeValidExhaustiveOverSmallCapacity) {
    // Every (start, count) pair, across word boundaries, against a bool-vector model.
    const idx_t cap = 200;
    for (idx_t start = 0; start <= cap; start += 1) {
        for (idx_t count = 0; start + count <= cap; count += (count < 70 ? 1 : 13)) {
            ValidityMask m(cap);
            m.SetAllInvalid(cap);
            m.SetRangeValid(start, count);
            for (idx_t i = 0; i < cap; i++) {
                ASSERT_EQ(m.IsValid(i), i >= start && i < start + count)
                    << "start=" << start << " count=" << count << " i=" << i;
            }
        }
    }
}

TEST(ValidityMask, SetRangeValidDoesNotDisturbNeighbours) {
    ValidityMask m(256);
    m.SetInvalid(9);
    m.SetInvalid(10);
    m.SetInvalid(100);
    m.SetInvalid(200);
    m.SetRangeValid(10, 91); // rows 10..100
    EXPECT_FALSE(m.IsValid(9));
    for (idx_t i = 10; i <= 100; i++)
        EXPECT_TRUE(m.IsValid(i)) << i;
    EXPECT_FALSE(m.IsValid(200));
}

TEST(ValidityMask, ResetRestoresAllValidAndResizes) {
    ValidityMask m(100);
    m.SetInvalid(10);
    m.Reset(500);
    EXPECT_TRUE(m.AllValid());
    EXPECT_EQ(m.capacity(), 500u);
    m.SetInvalid(499);
    EXPECT_FALSE(m.IsValid(499));
}

TEST(ValidityMask, SetAllValidDropsStorage) {
    ValidityMask m(100);
    m.SetInvalid(1);
    m.SetAllValid();
    EXPECT_TRUE(m.AllValid());
    EXPECT_TRUE(m.IsValid(1));
}

TEST(ValidityMask, ShallowCopySharesAllocatedStorage) {
    ValidityMask a(100);
    a.SetInvalid(1);
    ValidityMask b = a; // shares the allocated words
    a.SetInvalid(2);
    EXPECT_FALSE(b.IsValid(2)); // visible through the shared copy
}

TEST(ValidityMask, DeepCopyIsIndependent) {
    ValidityMask a(100);
    a.SetInvalid(1);
    ValidityMask b = a.DeepCopy();
    a.SetInvalid(2);
    b.SetInvalid(3);
    EXPECT_TRUE(b.IsValid(2));
    EXPECT_TRUE(a.IsValid(3));
    EXPECT_FALSE(b.IsValid(1));
    // deep copy of an all-valid mask stays unallocated
    EXPECT_TRUE(ValidityMask(50).DeepCopy().AllValid());
}

TEST(ValidityMask, MutableWordsAllocatesAllOnes) {
    ValidityMask m(130);
    uint64_t* w = m.MutableWords();
    EXPECT_FALSE(m.AllValid());
    EXPECT_EQ(w[0], ~uint64_t{0});
    EXPECT_EQ(w[1], ~uint64_t{0});
    EXPECT_EQ(m.CountValid(130), 130u);
}

// Randomised model check against std::vector<bool>.
TEST(ValidityMask, RandomOperationsMatchBoolVectorModel) {
    test::Rng rng(2024);
    for (idx_t cap : {idx_t{1}, idx_t{64}, idx_t{65}, idx_t{1000}, idx_t{2048}}) {
        ValidityMask m(cap);
        std::vector<bool> model(cap, true);
        for (int op = 0; op < 5000; op++) {
            const idx_t row = test::RandBelow(rng, cap);
            switch (test::RandBelow(rng, 5)) {
            case 4: {
                const idx_t len = test::RandBelow(rng, cap - row + 1);
                m.SetRangeValid(row, len);
                for (idx_t i = row; i < row + len; i++)
                    model[i] = true;
                break;
            }
            case 0:
                m.SetInvalid(row);
                model[row] = false;
                break;
            case 1:
                m.SetValid(row);
                model[row] = true;
                break;
            case 2:
                m.Set(row, test::Chance(rng, 0.5));
                model[row] = m.IsValid(row);
                break;
            default: {
                const idx_t count = test::RandBelow(rng, cap + 1);
                idx_t expect = 0;
                for (idx_t i = 0; i < count; i++)
                    expect += model[i];
                ASSERT_EQ(m.CountValid(count), expect);
            }
            }
            ASSERT_EQ(m.IsValid(row), model[row]);
        }
        for (idx_t i = 0; i < cap; i++)
            ASSERT_EQ(m.IsValid(i), model[i]);
    }
}

TEST(ValidityMask, FromBufferReadsExistingWords) {
    auto words = Buffer::Allocate(4 * sizeof(uint64_t));
    auto* w = words->As<uint64_t>();
    w[0] = ~uint64_t{0};
    w[1] = ~uint64_t{0} & ~(uint64_t{1} << 5); // row 69 invalid
    w[2] = 0;                                  // rows 128..191 invalid
    w[3] = ~uint64_t{0};
    ValidityMask m = ValidityMask::FromBuffer(words, 256);
    EXPECT_FALSE(m.AllValid());
    EXPECT_TRUE(m.IsValid(0));
    EXPECT_TRUE(m.IsValid(68));
    EXPECT_FALSE(m.IsValid(69));
    EXPECT_TRUE(m.IsValid(70));
    EXPECT_FALSE(m.IsValid(128));
    EXPECT_FALSE(m.IsValid(191));
    EXPECT_TRUE(m.IsValid(192));
    EXPECT_EQ(m.CountValid(256), 256u - 1 - 64);
}

TEST(ValidityMask, FromBufferOverAViewReadsAtTheViewOffset) {
    auto parent = Buffer::Allocate(4 * sizeof(uint64_t));
    auto* w = parent->As<uint64_t>();
    w[0] = ~uint64_t{0};
    w[1] = ~uint64_t{0};
    w[2] = ~uint64_t{0} & ~uint64_t{1}; // row 0 of the second half invalid
    w[3] = ~uint64_t{0};
    auto second_half = Buffer::View(parent, 2 * sizeof(uint64_t), 2 * sizeof(uint64_t));
    ValidityMask m = ValidityMask::FromBuffer(second_half, 128);
    EXPECT_FALSE(m.IsValid(0));
    EXPECT_TRUE(m.IsValid(1));
    EXPECT_EQ(m.CountValid(128), 127u);
}

TEST(ValidityMaskDeathTest, FromBufferRejectsATooSmallBuffer) {
    auto words = Buffer::Allocate(8);
    EXPECT_DEATH(ValidityMask::FromBuffer(words, 65), "CDB_CHECK");
}

#if defined(CDB_ENABLE_ASSERTS)
TEST(ValidityMaskDeathTest, MutatingAMaskOverReadOnlyStorageAborts) {
    auto parent = Buffer::Allocate(2 * sizeof(uint64_t));
    std::memset(parent->data(), 0xFF, parent->size());
    ValidityMask m = ValidityMask::FromBuffer(Buffer::View(parent, 0, parent->size()), 128);
    EXPECT_DEATH(m.SetInvalid(3), "CDB_ASSERT");
    EXPECT_DEATH(m.SetValid(3), "CDB_ASSERT");
    EXPECT_DEATH(m.SetRangeValid(0, 10), "CDB_ASSERT");
    EXPECT_DEATH(m.SetAllInvalid(10), "CDB_ASSERT");
    EXPECT_DEATH(m.MutableWords(), "CDB_ASSERT");
}
#endif

TEST(ValidityMask, ResizeOfUnallocatedMaskJustChangesCapacity) {
    ValidityMask m(10);
    m.Resize(5000);
    EXPECT_TRUE(m.AllValid());
    EXPECT_EQ(m.capacity(), 5000u);
    m.SetInvalid(4999);
    EXPECT_FALSE(m.IsValid(4999));
}

TEST(ValidityMask, ResizePreservesBitsAndNewRowsAreValid) {
    for (idx_t from : {idx_t{1}, idx_t{64}, idx_t{100}, idx_t{2048}}) {
        for (idx_t to : {from, from + 1, from + 64, from * 2 + 5, idx_t{10000}}) {
            ValidityMask m(from);
            test::Rng rng(from * 31 + to);
            std::vector<bool> model(to, true);
            for (idx_t i = 0; i < from; i++) {
                if (test::Chance(rng, 0.4)) {
                    m.SetInvalid(i);
                    model[i] = false;
                }
            }
            m.Resize(to);
            EXPECT_EQ(m.capacity(), to);
            for (idx_t i = 0; i < to; i++)
                ASSERT_EQ(m.IsValid(i), model[i]) << from << "->" << to;
            // and the grown mask is fully writable
            m.SetInvalid(to - 1);
            EXPECT_FALSE(m.IsValid(to - 1));
        }
    }
}

TEST(ValidityMaskDeathTest, ResizeCannotShrink) {
    ValidityMask m(100);
    EXPECT_DEATH(m.Resize(99), "CDB_CHECK");
}

} // namespace cdb
