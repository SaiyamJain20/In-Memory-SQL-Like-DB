#include "storage/checksum.h"

#include "kernels/cpu.h"

#include <array>
#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace cdb {

namespace {

constexpr uint32_t kPolynomial = 0x82F63B78u; // reflected Castagnoli

// Slicing-by-8 tables: kTables[0] is the classic byte table; kTables[k][i] is the CRC of byte i
// followed by k zero bytes.
constexpr std::array<std::array<uint32_t, 256>, 8> MakeTables() {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            c = (c & 1) ? (c >> 1) ^ kPolynomial : c >> 1;
        }
        t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        for (size_t k = 1; k < 8; k++) {
            t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        }
    }
    return t;
}
constexpr auto kTables = MakeTables();

uint32_t UpdatePortable(uint32_t crc, const uint8_t* p, size_t n) noexcept {
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8); // little-endian hosts only: the engine targets x86-64 / aarch64
        w ^= crc;
        crc = kTables[7][w & 0xFF] ^ kTables[6][(w >> 8) & 0xFF] ^ kTables[5][(w >> 16) & 0xFF] ^
              kTables[4][(w >> 24) & 0xFF] ^ kTables[3][(w >> 32) & 0xFF] ^
              kTables[2][(w >> 40) & 0xFF] ^ kTables[1][(w >> 48) & 0xFF] ^ kTables[0][w >> 56];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) {
        crc = kTables[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2"))) uint32_t UpdateHardware(uint32_t crc, const uint8_t* p,
                                                          size_t n) noexcept {
    uint64_t c = crc;
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        c = _mm_crc32_u64(c, w);
        p += 8;
        n -= 8;
    }
    auto c32 = static_cast<uint32_t>(c);
    while (n-- > 0) {
        c32 = _mm_crc32_u8(c32, *p++);
    }
    return c32;
}
#endif

} // namespace

uint32_t Crc32cPortable(const void* data, size_t size, uint32_t seed) noexcept {
    return ~UpdatePortable(~seed, static_cast<const uint8_t*>(data), size);
}

uint32_t Crc32c(const void* data, size_t size, uint32_t seed) noexcept {
#if defined(__x86_64__)
    if (kernels::UseSse42Crc()) {
        return ~UpdateHardware(~seed, static_cast<const uint8_t*>(data), size);
    }
#endif
    return Crc32cPortable(data, size, seed);
}

} // namespace cdb
