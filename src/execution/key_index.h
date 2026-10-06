#pragma once

#include "execution/chunk_store.h"

#include <vector>

namespace cdb {

// Equality of two values of one physical type as the engine defines it: bytes for strings, and
// for doubles Value::Compare's total order (0.0 == -0.0, NaN == NaN).
template <class T> inline bool KeyEquals(const T& a, const T& b) noexcept {
    if constexpr (std::is_same_v<T, double>) {
        return a == b || (a != a && b != b);
    } else if constexpr (std::is_same_v<T, string_t>) {
        return a ==
               b; // length + prefix in one compare, then the inline tail or the out-of-line bytes
    } else {
        return a == b;
    }
}

// Row-equality between stored rows and rows of input vectors, for hash tables.
//
// Prepared once per input chunk (UnifiedFormat per key column), then asked "is stored row r equal
// to input row i?" or "are input rows i and j equal?". NULLs compare equal to each other when
// `nulls_equal` (GROUP BY / DISTINCT); joins pre-filter NULL keys and pass false.
class KeyComparator {
  public:
    KeyComparator(const std::vector<const Vector*>& columns, bool nulls_equal);

    bool StoredEqualsInput(const ChunkStore& store, idx_t stored_row, idx_t input_row) const;
    bool InputEqualsInput(idx_t a, idx_t b) const;

  private:
    std::vector<UnifiedFormat> input_;
    std::vector<PhysicalType> types_;
    bool nulls_equal_;
};

// A hash index assigning dense ids 0, 1, 2, ... to distinct key tuples, in order of first
// appearance. The key tuples themselves are kept in a ChunkStore, so `keys().GetValue(c, id)`
// recovers the key of group `id`. NULLs are equal to each other. Open addressing with linear
// probing; the table doubles when it would exceed half full.
class KeyIndex {
  public:
    explicit KeyIndex(std::vector<LogicalType> key_types);

    // Number of distinct keys so far.
    idx_t Count() const noexcept { return hashes_.size(); }
    const ChunkStore& keys() const noexcept { return keys_; }

    // For each row i < count of `keys` (columns match the key types) stores its id in ids[i],
    // inserting unseen keys. Returns how many new keys were inserted; their input rows (in id
    // order) are appended to `new_rows` if it is not null.
    idx_t FindOrInsert(const DataChunk& keys, idx_t count, uint32_t* ids,
                       std::vector<sel_t>* new_rows = nullptr);

  private:
    void EnsureCapacity(idx_t extra);

    ChunkStore keys_;
    // Scratch for FindOrInsert, kept between calls so a hot loop does not allocate (and zero-fill).
    std::vector<uint64_t> scratch_hashes_;
    std::vector<sel_t> scratch_pending_;
    std::vector<const Vector*> scratch_columns_;
    std::vector<uint64_t> hashes_; // per id
    std::vector<uint32_t> slots_;  // 0 = empty, otherwise id + 1
    uint64_t mask_ = 0;
};

} // namespace cdb
