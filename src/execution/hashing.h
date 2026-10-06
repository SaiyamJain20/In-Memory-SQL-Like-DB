#pragma once

#include "vector/vector.h"

#include <cstddef>
#include <cstdint>

namespace cdb {

// Hash functions for hash aggregation, joins and DISTINCT.
//
// Hashing agrees with equality as the engine defines it (Value::Compare): -0.0 and 0.0 hash the
// same, every NaN hashes the same, strings hash by bytes, and NULL hashes to a fixed value (so
// GROUP BY / DISTINCT can put NULLs together; joins skip NULL keys before they get here).

uint64_t HashBytes(const char* data, size_t length) noexcept;

// A bijective 64-bit mixer (the murmur3 finaliser).
inline uint64_t HashInt64(uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

inline uint64_t CombineHash(uint64_t seed, uint64_t h) noexcept {
    return HashInt64(seed * 0x9e3779b97f4a7c15ULL + h);
}

// Hashes logical rows [0, count) of `v` (any vector format). With `combine` false the hashes are
// written to out[0..count); with it true they are mixed into the values already there.
void HashVector(const Vector& v, idx_t count, uint64_t* out, bool combine);

// Hashes the rows of several key columns into one hash per row.
void HashColumns(const Vector* const* columns, size_t column_count, idx_t count, uint64_t* out);

} // namespace cdb
