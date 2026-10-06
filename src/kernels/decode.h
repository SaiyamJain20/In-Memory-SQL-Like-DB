#pragma once

#include "common/types.h"

#include <cstdint>

namespace cdb::kernels {

// Conversions at the end of bit-packed decoding: `off` holds unpacked offsets (non-negative), the
// result is base + offset. Exact for every input; the scaled-double conversion has an AVX2 version
// that is bit for bit equal to the scalar one. (The integer conversions are plain loops: the
// compiler vectorises them better than hand-written AVX2 does.)

// out[i] = (int32_t)(base + off[i]) (wrapping arithmetic, like the encoder's).
void OffsetsToInt32(const uint64_t* off, idx_t n, uint64_t base, int32_t* out);
void OffsetsToInt64(const uint64_t* off, idx_t n, uint64_t base, int64_t* out);

// out[i] = (double)(int64_t)(base + off[i]) / scale. Requires off[i] < 2^52 and |base + off[i]| <
// 2^53 (the scaled-double encoder guarantees the second; callers check the first via the bit
// width), which makes every intermediate exactly representable so the AVX2 version (which builds
// the double from the offset's bits and adds the base in floating point) gives the same bits as the
// scalar one.
void OffsetsToScaledDouble(const uint64_t* off, idx_t n, int64_t base, double scale, double* out);

} // namespace cdb::kernels
