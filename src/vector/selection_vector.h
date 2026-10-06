#pragma once

#include "common/assert.h"
#include "common/types.h"
#include "memory/buffer.h"

#include <memory>

namespace cdb {

// An array of row indices selecting (and reordering / repeating) rows of a vector. Filters
// produce one instead of copying data; dictionary vectors are "child + selection vector".
//
// Copying is SHALLOW (shares the index array); use Copy() for an independent array.
// Identity() and Zeros() are shared read-only views; writing to them is a bug.
class SelectionVector {
  public:
    SelectionVector() = default;

    // Owning vector of `count` indices, zero-initialised.
    explicit SelectionVector(idx_t count)
        : owner_(Buffer::Allocate(count * sizeof(sel_t))), data_(owner_->As<sel_t>()),
          capacity_(count) {}

    // Owning vector of `count` indices that are NOT initialised: for producers that write every
    // entry they hand out before anything reads it (zeroing an array that is about to be
    // overwritten was measurable in join-heavy queries). The public constructor above stays
    // zero-filled.
    static SelectionVector Uninitialized(idx_t count) {
        SelectionVector s;
        s.owner_ = Buffer::AllocateUninitialized(count * sizeof(sel_t));
        s.data_ = s.owner_->As<sel_t>();
        s.capacity_ = count;
        return s;
    }

    // Read-only [0, 1, 2, ..., kVectorSize-1].
    static const SelectionVector& Identity();
    // Read-only [0, 0, 0, ...] of length kVectorSize; the selection of a constant vector.
    static const SelectionVector& Zeros();

    bool IsSet() const noexcept { return data_ != nullptr; }
    idx_t capacity() const noexcept { return capacity_; }

    sel_t operator[](idx_t i) const noexcept {
        CDB_ASSERT(i < capacity_);
        return data_[i];
    }
    void Set(idx_t i, sel_t value) noexcept {
        CDB_ASSERT(owner_ != nullptr && i < capacity_);
        data_[i] = value;
    }

    const sel_t* data() const noexcept { return data_; }
    sel_t* MutableData() noexcept {
        CDB_ASSERT(owner_ != nullptr);
        return data_;
    }

    // Independent copy of the first `count` indices.
    SelectionVector Copy(idx_t count) const;

  private:
    SelectionVector(const sel_t* shared_static, idx_t capacity)
        : data_(const_cast<sel_t*>(shared_static)), capacity_(capacity) {}

    std::shared_ptr<Buffer> owner_; // null for the static Identity()/Zeros() views
    sel_t* data_ = nullptr;
    idx_t capacity_ = 0;
};

} // namespace cdb
