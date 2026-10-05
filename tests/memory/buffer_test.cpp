#include "memory/buffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace cdb {

TEST(Buffer, IsAlignedAndZeroInitialised) {
    for (size_t bytes : {size_t{0}, size_t{1}, size_t{7}, size_t{63}, size_t{64}, size_t{65},
                         size_t{4096}, size_t{100000}}) {
        auto buf = Buffer::Allocate(bytes);
        ASSERT_NE(buf->data(), nullptr) << bytes;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(buf->data()) % Buffer::kAlignment, 0u) << bytes;
        EXPECT_EQ(buf->size(), bytes);
        // The logical region AND the padding must read as zero.
        const size_t readable = bytes + Buffer::kPadding;
        for (size_t i = 0; i < readable; i++) {
            ASSERT_EQ(buf->data()[i], 0) << "byte " << i << " of " << bytes;
        }
    }
}

TEST(Buffer, PaddingIsWritable) {
    // SIMD tail stores may spill into the padding; under ASan this must not be flagged.
    auto buf = Buffer::Allocate(10);
    std::memset(buf->data(), 0xAB, 10 + Buffer::kPadding);
    EXPECT_EQ(buf->data()[10 + Buffer::kPadding - 1], 0xAB);
}

TEST(Buffer, TypedAccess) {
    auto buf = Buffer::Allocate(4 * sizeof(int64_t));
    int64_t* p = buf->As<int64_t>();
    for (int i = 0; i < 4; i++)
        p[i] = i * 1000;
    const Buffer& cbuf = *buf;
    EXPECT_EQ(cbuf.As<int64_t>()[3], 3000);
}

TEST(Buffer, SharedOwnershipKeepsDataAlive) {
    std::shared_ptr<Buffer> a = Buffer::Allocate(128);
    a->data()[5] = 42;
    std::shared_ptr<Buffer> b = a;
    a.reset();
    EXPECT_EQ(b->data()[5], 42);
    EXPECT_EQ(b.use_count(), 1);
}

TEST(BufferView, WindowsIntoTheParentWithoutCopying) {
    auto parent = Buffer::Allocate(256);
    for (int i = 0; i < 256; i++)
        parent->data()[i] = static_cast<uint8_t>(i);
    auto view = Buffer::View(parent, 64, 32);
    EXPECT_TRUE(view->read_only());
    EXPECT_FALSE(parent->read_only());
    EXPECT_EQ(view->size(), 32u);
    EXPECT_EQ(view->data(), parent->data() + 64);
    EXPECT_EQ(view->data()[0], 64);
    EXPECT_EQ(view->data()[31], 95);
}

TEST(BufferView, KeepsTheParentAlive) {
    std::shared_ptr<Buffer> view;
    {
        auto parent = Buffer::Allocate(128);
        parent->data()[100] = 0x7E;
        view = Buffer::View(parent, 96, 32);
    } // the only remaining reference to the parent is held by the view
    EXPECT_EQ(view->data()[4], 0x7E); // ASan: no use-after-free
}

TEST(BufferView, ViewsOfViewsAndFullWindows) {
    auto parent = Buffer::Allocate(100);
    parent->data()[50] = 9;
    auto v1 = Buffer::View(parent, 10, 80);
    auto v2 = Buffer::View(v1, 40, 10);
    EXPECT_EQ(v2->data()[0], 9);
    EXPECT_TRUE(v2->read_only());
    auto whole = Buffer::View(parent, 0, 100);
    EXPECT_EQ(whole->data(), parent->data());
    auto empty = Buffer::View(parent, 100, 0);
    EXPECT_EQ(empty->size(), 0u);
}

TEST(BufferViewDeathTest, WindowMustLieInsideTheParent) {
    auto parent = Buffer::Allocate(64);
    EXPECT_DEATH(Buffer::View(parent, 32, 33), "CDB_CHECK");
    EXPECT_DEATH(Buffer::View(parent, 65, 0), "CDB_CHECK");
    EXPECT_DEATH(Buffer::View(nullptr, 0, 0), "CDB_CHECK");
}

} // namespace cdb
