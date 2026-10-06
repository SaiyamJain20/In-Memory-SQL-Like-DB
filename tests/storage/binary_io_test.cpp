#include "storage/binary_io.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>

namespace cdb {

namespace {
BinaryReader ReaderOf(const BinaryWriter& w) {
    return BinaryReader(w.buffer().data(), w.buffer().size(), "test data");
}
void ExpectCorruption(const std::function<void()>& fn) {
    try {
        fn();
        FAIL() << "expected a Corruption error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption) << e.what();
        EXPECT_NE(std::string(e.what()).find("test data"), std::string::npos)
            << "the message names what was being read: " << e.what();
    }
}
} // namespace

TEST(BinaryIo, RoundTripsEveryType) {
    BinaryWriter w;
    w.U8(7);
    w.U32(0xDEADBEEFu);
    w.U64(0x0123456789ABCDEFull);
    w.I64(std::numeric_limits<int64_t>::min());
    w.F64(-0.0);
    w.F64(std::nan("0x123"));
    w.String("hello");
    w.String("");
    w.String(std::string("a\0b", 3));
    BinaryReader r = ReaderOf(w);
    EXPECT_EQ(r.U8(), 7);
    EXPECT_EQ(r.U32(), 0xDEADBEEFu);
    EXPECT_EQ(r.U64(), 0x0123456789ABCDEFull);
    EXPECT_EQ(r.I64(), std::numeric_limits<int64_t>::min());
    const double nz = r.F64();
    EXPECT_EQ(nz, 0.0);
    EXPECT_TRUE(std::signbit(nz)) << "-0.0 keeps its sign";
    const double nan = r.F64();
    uint64_t bits, want_bits;
    const double expect_nan = std::nan("0x123");
    std::memcpy(&bits, &nan, 8);
    std::memcpy(&want_bits, &expect_nan, 8);
    EXPECT_EQ(bits, want_bits) << "NaN payloads survive";
    EXPECT_EQ(r.String(), "hello");
    EXPECT_EQ(r.String(), "");
    EXPECT_EQ(r.String(), std::string_view("a\0b", 3));
    EXPECT_TRUE(r.AtEnd());
    r.ExpectEnd();
}

TEST(BinaryIo, LayoutIsLittleEndianAndFixedWidth) {
    BinaryWriter w;
    w.U32(0x01020304u);
    w.U64(1);
    ASSERT_EQ(w.size(), 12U);
    const std::vector<uint8_t> want = {4, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(w.buffer(), want);
}

TEST(BinaryIo, PatchFillsInAReservedLength) {
    BinaryWriter w;
    const size_t at = w.ReserveU32();
    w.Bytes("abcdef", 6);
    w.PatchU32(at, static_cast<uint32_t>(w.size() - at - 4));
    BinaryReader r = ReaderOf(w);
    EXPECT_EQ(r.U32(), 6U);
}

TEST(BinaryIo, ReadingPastTheEndIsCorruptionNotUndefinedBehaviour) {
    BinaryWriter w;
    w.U32(5);
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(w);
        r.U32();
        r.U8();
    });
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(w);
        r.U64();
    });
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(w);
        r.Bytes(5);
    });
    // a string whose length says more than there is
    BinaryWriter s;
    s.U32(1000);
    s.Bytes("abc", 3);
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(s);
        r.String();
    });
    // the empty buffer
    ExpectCorruption([] {
        BinaryReader r(nullptr, 0, "test data");
        r.U8();
    });
}

TEST(BinaryIo, ACountThatCannotFitIsRejectedBeforeAnythingIsAllocated) {
    BinaryWriter w;
    w.U32(0xFFFFFFFFu);
    w.U64(0);
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(w);
        r.Count(8);
    });
    BinaryWriter ok;
    ok.U32(2);
    ok.U64(1);
    ok.U64(2);
    BinaryReader r = ReaderOf(ok);
    EXPECT_EQ(r.Count(8), 2U);
    BinaryWriter exact;
    exact.U32(1);
    exact.U64(1);
    BinaryReader r2 = ReaderOf(exact);
    EXPECT_EQ(r2.Count(8), 1U) << "exactly enough room is fine";
    BinaryWriter one_short;
    one_short.U32(2);
    one_short.U64(1);
    ExpectCorruption([&] {
        BinaryReader r3 = ReaderOf(one_short);
        r3.Count(8);
    });
}

TEST(BinaryIo, ExpectEndRejectsTrailingBytes) {
    BinaryWriter w;
    w.U32(1);
    w.U8(0);
    ExpectCorruption([&] {
        BinaryReader r = ReaderOf(w);
        r.U32();
        r.ExpectEnd();
    });
}

} // namespace cdb
