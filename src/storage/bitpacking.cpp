#include "storage/bitpacking.h"

#include <array>
#include <cstring>
#include <utility>

namespace cdb {

void BitPack(const uint64_t* in, idx_t count, uint8_t width, uint8_t* out) {
    if (width == 0) {
        return;
    }
    const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    uint64_t word = 0; // the word being assembled
    unsigned used = 0; // bits already placed in it
    size_t at = 0;     // byte offset of the next word to write
    for (idx_t i = 0; i < count; i++) {
        const uint64_t v = in[i] & mask;
        word |= v << used;
        if (used + width >= 64) {
            std::memcpy(out + at, &word, 8);
            at += 8;
            const unsigned taken = 64 - used; // bits of v that went into the finished word
            word = taken < 64 ? v >> taken : 0;
            used = used + width - 64;
        } else {
            used += width;
        }
    }
    if (used > 0) {
        std::memcpy(out + at, &word, 8);
    }
}

namespace {

// Generic unpack of values [first, count): one value at a time, any width. Used for tails.
void UnpackGeneric(const uint8_t* in, idx_t first, idx_t count, uint8_t width, uint64_t* out) {
    const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    for (idx_t i = first; i < count; i++) {
        const idx_t bit = i * width;
        const size_t byte = static_cast<size_t>(bit >> 6) * 8;
        const unsigned shift = static_cast<unsigned>(bit & 63);
        uint64_t lo, hi;
        std::memcpy(&lo, in + byte, 8);
        std::memcpy(&hi, in + byte + 8, 8);
        uint64_t v = lo >> shift;
        if (shift + width > 64) {
            v |= hi << (64 - shift);
        }
        out[i] = v & mask;
    }
}

// Value K of a block of 64 packed with width W: every position is a compile-time constant, so the
// compiler emits one shift, an optional second shift + or, and a mask per value, with no loop and
// no branches. 64 values of W bits occupy exactly W words, so a block never reads past its own
// words.
template <unsigned W, unsigned K> inline uint64_t ExtractValue(const uint64_t* w) noexcept {
    constexpr unsigned bit = K * W;
    constexpr unsigned word = bit / 64;
    constexpr unsigned shift = bit % 64;
    constexpr uint64_t mask = W == 64 ? ~uint64_t{0} : (uint64_t{1} << W) - 1;
    uint64_t v = w[word] >> shift;
    if constexpr (shift + W > 64) {
        v |= w[word + 1] << (64 - shift);
    }
    return v & mask;
}

template <unsigned W, unsigned... K>
inline void UnpackBlock(const uint64_t* w, uint64_t* out,
                        std::integer_sequence<unsigned, K...>) noexcept {
    ((out[K] = ExtractValue<W, K>(w)), ...);
}

template <unsigned W> void UnpackWidth(const uint8_t* in, idx_t count, uint64_t* out) {
    idx_t i = 0;
    for (; i + 64 <= count; i += 64) {
        uint64_t words[W];
        std::memcpy(words, in + (i / 64) * W * 8, W * 8); // alignment-safe load of the block
        UnpackBlock<W>(words, out + i, std::make_integer_sequence<unsigned, 64>{});
    }
    if (i < count) {
        UnpackGeneric(in, i, count, W, out);
    }
}

using UnpackFn = void (*)(const uint8_t*, idx_t, uint64_t*);

template <unsigned... W>
constexpr std::array<UnpackFn, 64> MakeUnpackTable(std::integer_sequence<unsigned, W...>) {
    return {&UnpackWidth<W + 1>...}; // index w-1 holds the unpacker for width w (1..64)
}

constexpr std::array<UnpackFn, 64> kUnpackTable =
    MakeUnpackTable(std::make_integer_sequence<unsigned, 64>{});

} // namespace

void BitUnpack(const uint8_t* in, idx_t count, uint8_t width, uint64_t* out) {
    if (width == 0) {
        std::memset(out, 0, count * sizeof(uint64_t));
        return;
    }
    if (width == 64) {
        std::memcpy(out, in, count * sizeof(uint64_t));
        return;
    }
    kUnpackTable[width - 1](in, count, out);
}

} // namespace cdb
