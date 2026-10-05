#pragma once

#include "common/types.h"
#include "memory/buffer.h"

#include <memory>

namespace cdb {

// Per-row validity (NULL tracking) as a bitmask: bit set = valid, bit clear = NULL.
//
// A mask with no storage means "every row is valid". That is the common case, and it costs a
// pointer check instead of a bit test per row. Storage is allocated lazily on the first
// SetInvalid().
//
// Copying a ValidityMask is SHALLOW: a copy shares the owner's storage if it has been allocated
// (which is what makes Vector::Reference cheap), so only the producer may mutate a mask that has
// been shared. A copy of a mask that has no storage yet allocates its own on first write. Use
// DeepCopy() for a fully independent mask.
class ValidityMask {
  public:
    ValidityMask() = default;
    explicit ValidityMask(idx_t capacity) : capacity_(capacity) {}

    idx_t capacity() const noexcept { return capacity_; }

    // True iff no row has ever been marked invalid (no storage allocated).
    bool AllValid() const noexcept { return buffer_ == nullptr; }

    bool IsValid(idx_t row) const noexcept {
        return buffer_ == nullptr || ((Words()[row >> 6] >> (row & 63)) & 1) != 0;
    }

    void SetInvalid(idx_t row);
    void SetValid(idx_t row); // no-op when AllValid()
    void Set(idx_t row, bool valid) {
        if (valid) {
            SetValid(row);
        } else {
            SetInvalid(row);
        }
    }

    // Marks rows [start, start + count) valid. No-op when AllValid().
    void SetRangeValid(idx_t start, idx_t count);

    // Marks rows [0, count) invalid / valid.
    void SetAllInvalid(idx_t count);
    void SetAllValid() noexcept { buffer_.reset(); }

    // Drops storage and resizes the mask: afterwards every row up to `capacity` is valid.
    void Reset(idx_t capacity) noexcept {
        buffer_.reset();
        capacity_ = capacity;
    }

    // Number of valid rows among the first `count`.
    idx_t CountValid(idx_t count) const noexcept;

    // Number of 64-bit words needed for `bits` bits.
    static constexpr idx_t WordCount(idx_t bits) noexcept { return (bits + 63) / 64; }

    // Raw words, or nullptr when AllValid(). Bits at positions >= capacity are unspecified.
    const uint64_t* Words() const noexcept { return buffer_ ? buffer_->As<uint64_t>() : nullptr; }

    // Allocates storage (all valid) if absent, then returns writable words.
    uint64_t* MutableWords();

    ValidityMask DeepCopy() const;

  private:
    std::shared_ptr<Buffer> buffer_;
    idx_t capacity_ = kVectorSize;
};

} // namespace cdb
