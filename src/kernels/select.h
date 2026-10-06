#pragma once

#include "common/types.h"

#include <cstdint>

namespace cdb::kernels {

enum class CmpOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

// Writes the indices i < count with `data[i] <op> constant` to out[0..], in increasing order, and
// returns how many. `data` is a contiguous array (a Flat vector's rows); NULLs are the caller's
// business. Comparison follows the engine's order: for doubles -0.0 == 0.0 and NaN equals NaN and
// is greater than every number. `out` needs room for `count` entries and is not written past them.
// Instantiated for int32_t (also DATE), int64_t and double; AVX2 when the CPU has it.
template <class T>
idx_t SelectConstant(CmpOp op, const T* data, idx_t count, T constant, sel_t* out);

} // namespace cdb::kernels
