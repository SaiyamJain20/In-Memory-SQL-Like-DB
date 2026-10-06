#pragma once

#include "common/types.h"

#include <cstdint>

namespace cdb::kernels {

// Vectorised row hashing for integer columns. The results are exactly those of the scalar
// HashInt64(static_cast<uint64_t>(static_cast<int64_t>(x))) (and CombineHash for the combining
// form), so a column hashes the same whichever path produced it.

// out[i] = hash of data[i].
void HashInt32Column(const int32_t* data, idx_t n, uint64_t* out);
void HashInt64Column(const int64_t* data, idx_t n, uint64_t* out);

// out[i] = CombineHash(out[i], hash of data[i]).
void CombineInt32Column(const int32_t* data, idx_t n, uint64_t* out);
void CombineInt64Column(const int64_t* data, idx_t n, uint64_t* out);

} // namespace cdb::kernels
