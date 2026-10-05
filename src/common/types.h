#pragma once

#include <cstddef>
#include <cstdint>

namespace cdb {

// Row counts, row indices, byte counts. 64-bit so tables are not limited to 4G rows.
using idx_t = uint64_t;

// Index into a vector. 32 bits is plenty for a vector (<= kVectorSize rows) and halves the
// memory traffic of selection vectors compared to idx_t.
using sel_t = uint32_t;

// Maximum number of rows in a Vector / DataChunk. See docs/ARCHITECTURE.md for the rationale.
inline constexpr idx_t kVectorSize = 2048;

inline constexpr idx_t kInvalidIndex = ~idx_t{0};

constexpr size_t AlignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace cdb
