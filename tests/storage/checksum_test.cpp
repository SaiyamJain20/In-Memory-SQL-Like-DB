#include "kernels/cpu.h"
#include "storage/checksum.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace cdb {

namespace {
uint32_t Both(const void* d, size_t n) {
    const uint32_t portable = Crc32cPortable(d, n);
    EXPECT_EQ(Crc32c(d, n), portable) << "n = " << n;
    return portable;
}
} // namespace

TEST(Crc32c, StandardCheckValues) {
    EXPECT_EQ(Both("123456789", 9), 0xE3069283u);
    EXPECT_EQ(Both("", 0), 0u);
    // RFC 3720 (iSCSI) appendix B.4 test vectors
    const std::vector<uint8_t> zeros(32, 0), ones(32, 0xFF);
    EXPECT_EQ(Both(zeros.data(), 32), 0x8A9136AAu);
    EXPECT_EQ(Both(ones.data(), 32), 0x62A8AB43u);
    std::vector<uint8_t> up(32), down(32);
    for (int i = 0; i < 32; i++) {
        up[i] = static_cast<uint8_t>(i);
        down[i] = static_cast<uint8_t>(31 - i);
    }
    EXPECT_EQ(Both(up.data(), 32), 0x46DD794Eu);
    EXPECT_EQ(Both(down.data(), 32), 0x113FDB5Cu);
}

TEST(Crc32c, HardwareAndPortableAgreeOnEveryLengthAndAlignment) {
    std::mt19937_64 rng(5);
    std::vector<uint8_t> buf(700);
    for (uint8_t& b : buf) {
        b = static_cast<uint8_t>(rng());
    }
    for (size_t offset = 0; offset < 9; offset++) {
        for (size_t n = 0; n <= 300; n++) {
            Both(buf.data() + offset, n);
        }
    }
    for (const size_t n : {size_t{511}, size_t{512}, size_t{513}, size_t{690}}) {
        Both(buf.data(), n);
    }
}

TEST(Crc32c, SimdSwitchDoesNotChangeTheResult) {
    std::vector<uint8_t> buf(1000, 0x5A);
    kernels::SetSimdEnabled(true);
    const uint32_t on = Crc32c(buf.data(), buf.size());
    kernels::SetSimdEnabled(false);
    const uint32_t off = Crc32c(buf.data(), buf.size());
    kernels::SetSimdEnabled(true);
    EXPECT_EQ(on, off);
}

TEST(Crc32c, ChainingEqualsTheChecksumOfTheConcatenation) {
    std::mt19937_64 rng(6);
    std::vector<uint8_t> buf(500);
    for (uint8_t& b : buf) {
        b = static_cast<uint8_t>(rng());
    }
    const uint32_t whole = Crc32c(buf.data(), buf.size());
    for (const size_t cut :
         {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{250}, size_t{500}}) {
        const uint32_t first = Crc32c(buf.data(), cut);
        EXPECT_EQ(Crc32c(buf.data() + cut, buf.size() - cut, first), whole) << cut;
    }
}

TEST(Crc32c, EveryBitFlipChangesTheChecksum) {
    std::vector<uint8_t> buf(64);
    for (size_t i = 0; i < buf.size(); i++) {
        buf[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    const uint32_t base = Crc32c(buf.data(), buf.size());
    for (size_t byte = 0; byte < buf.size(); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= static_cast<uint8_t>(1 << bit);
            EXPECT_NE(Crc32c(buf.data(), buf.size()), base) << byte << ":" << bit;
            buf[byte] ^= static_cast<uint8_t>(1 << bit);
        }
    }
}

} // namespace cdb
