#pragma once

#include "common/types.h"

#include <cstddef>
#include <cstdint>

namespace cdb {

// Fixed-width bit packing: `count` unsigned values of `width` bits each (0..64) stored back to
// back, least significant bit first, in little-endian 64-bit words.
//
// A packed stream occupies PackedBytes(count, width) bytes: the bits rounded up to whole words plus
// one more word of slack, because unpacking reads the word after the one holding a value's first
// bit. Pack() needs the destination zeroed beforehand.

// Number of bits needed to represent x (0 for 0).
inline uint8_t BitWidth(uint64_t x) noexcept {
    return x == 0 ? uint8_t{0} : static_cast<uint8_t>(64 - __builtin_clzll(x));
}

constexpr size_t PackedBytes(idx_t count, uint8_t width) noexcept {
    return static_cast<size_t>((count * width + 63) / 64) * 8 + 8;
}

// Stores the low `width` bits of each in[i]. `out` must be zeroed and PackedBytes(count, width)
// long.
void BitPack(const uint64_t* in, idx_t count, uint8_t width, uint8_t* out);

// Reads `count` values back. `in` must be PackedBytes(count, width) long (the slack word included).
void BitUnpack(const uint8_t* in, idx_t count, uint8_t width, uint64_t* out);

} // namespace cdb
