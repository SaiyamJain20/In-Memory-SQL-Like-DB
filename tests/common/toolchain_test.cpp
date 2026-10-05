// Guards the C++20 features this project relies on, so a compiler / standard-library gap shows
// up as a failing test on the matrix (gcc + clang) rather than as a confusing error later.
#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

template <std::integral T> constexpr int PopCount(T v) {
    return std::popcount(static_cast<std::make_unsigned_t<T>>(v));
}

} // namespace

TEST(Toolchain, Concepts) {
    static_assert(PopCount(uint64_t{0xFF}) == 8);
    EXPECT_EQ(PopCount(0b1011), 3);
}

TEST(Toolchain, SpanOverVector) {
    std::vector<int> v{1, 2, 3, 4};
    std::span<int> s(v);
    EXPECT_EQ(s.subspan(1, 2)[0], 2);
}

TEST(Toolchain, Format) {
    EXPECT_EQ(std::format("{}-{:04}", "q", 7), "q-0007");
}

TEST(Toolchain, BitOps) {
    EXPECT_EQ(std::countr_zero(uint32_t{8}), 3);
    EXPECT_EQ(std::bit_ceil(uint32_t{1000}), 1024u);
}

TEST(Toolchain, JThreadAndAtomicWait) {
    std::atomic<int> flag{0};
    std::jthread t([&] {
        flag.store(1);
        flag.notify_all();
    });
    flag.wait(0);
    EXPECT_EQ(flag.load(), 1);
}
