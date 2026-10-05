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

} // namespace cdb
