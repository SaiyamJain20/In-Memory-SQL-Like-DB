#include "vector/selection_vector.h"

#include <gtest/gtest.h>

namespace cdb {

TEST(SelectionVector, DefaultIsUnset) {
    SelectionVector s;
    EXPECT_FALSE(s.IsSet());
    EXPECT_EQ(s.capacity(), 0u);
}

TEST(SelectionVector, OwningVectorIsZeroInitialisedAndWritable) {
    SelectionVector s(10);
    ASSERT_TRUE(s.IsSet());
    EXPECT_EQ(s.capacity(), 10u);
    for (idx_t i = 0; i < 10; i++)
        EXPECT_EQ(s[i], 0u);
    for (idx_t i = 0; i < 10; i++)
        s.Set(i, static_cast<sel_t>(100 - i));
    for (idx_t i = 0; i < 10; i++)
        EXPECT_EQ(s[i], 100 - i);
    s.MutableData()[3] = 7;
    EXPECT_EQ(s[3], 7u);
}

TEST(SelectionVector, IdentityIsZeroToVectorSizeMinusOne) {
    const SelectionVector& id = SelectionVector::Identity();
    ASSERT_TRUE(id.IsSet());
    EXPECT_EQ(id.capacity(), kVectorSize);
    for (idx_t i = 0; i < kVectorSize; i++)
        ASSERT_EQ(id[i], i);
    // singleton: the same storage every time
    EXPECT_EQ(SelectionVector::Identity().data(), id.data());
}

TEST(SelectionVector, ZerosIsAllZero) {
    const SelectionVector& z = SelectionVector::Zeros();
    EXPECT_EQ(z.capacity(), kVectorSize);
    for (idx_t i = 0; i < kVectorSize; i++)
        ASSERT_EQ(z[i], 0u);
    EXPECT_NE(z.data(), SelectionVector::Identity().data());
}

TEST(SelectionVector, ShallowCopySharesStorage) {
    SelectionVector a(4);
    a.Set(0, 9);
    SelectionVector b = a;
    EXPECT_EQ(a.data(), b.data());
    b.Set(1, 5);
    EXPECT_EQ(a[1], 5u);
}

TEST(SelectionVector, CopyIsIndependentAndTruncates) {
    SelectionVector a(5);
    for (idx_t i = 0; i < 5; i++)
        a.Set(i, static_cast<sel_t>(i * 10));
    SelectionVector c = a.Copy(3);
    EXPECT_EQ(c.capacity(), 3u);
    EXPECT_NE(c.data(), a.data());
    for (idx_t i = 0; i < 3; i++)
        EXPECT_EQ(c[i], a[i]);
    c.Set(0, 99);
    EXPECT_EQ(a[0], 0u);
    // copying zero entries is valid
    EXPECT_EQ(a.Copy(0).capacity(), 0u);
}

TEST(SelectionVector, CopyOfIdentityBecomesWritable) {
    SelectionVector c = SelectionVector::Identity().Copy(8);
    c.Set(0, 5);
    EXPECT_EQ(c[0], 5u);
    EXPECT_EQ(SelectionVector::Identity()[0], 0u); // the singleton is untouched
}

} // namespace cdb
