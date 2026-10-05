#pragma once

#include "common/assert.h"
#include "common/types.h"
#include "memory/buffer.h"
#include "types/logical_type.h"
#include "types/string_t.h"
#include "types/value.h"
#include "vector/selection_vector.h"
#include "vector/string_heap.h"
#include "vector/validity_mask.h"

#include <memory>
#include <string_view>

namespace cdb {

enum class VectorFormat : uint8_t {
    Flat,       // one value per row, contiguous
    Constant,   // a single value logically repeated for every row
    Dictionary, // selection vector into a flat child vector
};

// A format-independent view of a vector's contents for kernels: logical row `i` lives at
// `data[sel[i]]` with validity bit `validity->IsValid(sel[i])`. Valid only while the source
// vector is alive and unmodified.
struct UnifiedFormat {
    const sel_t* sel = nullptr;
    const uint8_t* data = nullptr;
    const ValidityMask* validity = nullptr;

    template <class T> const T* Data() const noexcept { return reinterpret_cast<const T*>(data); }
    bool IsValid(idx_t row) const noexcept { return validity->IsValid(sel[row]); }
};

// A column of up to `capacity` (<= kVectorSize) values of one logical type.
//
// Ownership model: buffers (data, validity, selection, string heap) are reference counted so
// that Reference() and Slice() are O(1) and zero-copy. A Vector may be written only through the
// Flat-format accessors, and only by whoever produced it; code that has Reference()d or
// Slice()d a vector must not mutate the shared buffers. Reset() is safe: it never clobbers
// buffers that another vector still holds.
//
// Contents at positions >= the producer's row count are unspecified.
class Vector {
  public:
    // A flat vector of `capacity` rows, all valid, zero-initialised.
    explicit Vector(LogicalType type, idx_t capacity = kVectorSize);

    // A constant vector holding `value`.
    static Vector MakeConstant(const Value& value, idx_t capacity = kVectorSize);

    Vector(Vector&&) noexcept = default;
    Vector& operator=(Vector&&) noexcept = default;
    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

    LogicalType type() const noexcept { return type_; }
    VectorFormat format() const noexcept { return format_; }
    idx_t capacity() const noexcept { return capacity_; }

    // ---- Flat-format access -----------------------------------------------------------
    // Raw element array. Requires format() == Flat and sizeof(T)/physical type to match.
    template <class T> T* FlatData() {
        CDB_ASSERT(format_ == VectorFormat::Flat && PhysicalTypeOf<T>::value == type_.physical());
        return data_->As<T>();
    }
    template <class T> const T* FlatData() const {
        CDB_ASSERT(format_ == VectorFormat::Flat && PhysicalTypeOf<T>::value == type_.physical());
        return data_->As<T>();
    }

    // Validity of the Flat vector (per row) or Constant vector (bit 0).
    ValidityMask& Validity() {
        CDB_ASSERT(format_ == VectorFormat::Flat || format_ == VectorFormat::Constant);
        return validity_;
    }
    const ValidityMask& Validity() const {
        CDB_ASSERT(format_ == VectorFormat::Flat || format_ == VectorFormat::Constant);
        return validity_;
    }

    // The heap that owns this (Flat or Constant) VARCHAR vector's out-of-line bytes.
    StringHeap& Heap();
    // Copies `value` into this vector's heap (or inlines it) and returns the string_t.
    string_t AddString(std::string_view value) { return Heap().Add(value); }

    // ---- Dictionary access ------------------------------------------------------------
    const SelectionVector& DictionarySel() const {
        CDB_ASSERT(format_ == VectorFormat::Dictionary);
        return sel_;
    }
    const Vector& DictionaryChild() const {
        CDB_ASSERT(format_ == VectorFormat::Dictionary);
        return *child_;
    }

    // ---- Generic (slow-path) access, any format ---------------------------------------
    Value GetValue(idx_t row) const;
    // Flat only. Strings are copied into this vector's heap.
    void SetValue(idx_t row, const Value& value);

    // ---- Structural operations --------------------------------------------------------
    // Back to an empty Flat vector with every row valid. Safe if buffers are shared.
    void Reset();

    // Makes this vector share all of `other`'s buffers (shallow, O(1)).
    void Reference(const Vector& other);

    // Becomes a Constant vector holding `value`.
    void SetConstant(const Value& value);

    // Converts to Flat, materialising the first `count` rows. No-op if already Flat.
    void Flatten(idx_t count);

    // Replaces this vector with rows `sel[0..count)` of itself, without copying data: Flat
    // becomes Dictionary, Dictionary composes selections (depth stays 1), Constant is a no-op.
    void Slice(const SelectionVector& sel, idx_t count);

    void ToUnified(UnifiedFormat& out) const;

    // Aborts (CDB_CHECK) if internal invariants are violated for the first `count` rows:
    // buffer sizes, dictionary indices in range, string_t zero-padding / pointer validity.
    void Verify(idx_t count) const;

  private:
    struct NoAllocTag {};
    Vector(LogicalType type, idx_t capacity, NoAllocTag) noexcept
        : type_(type), capacity_(capacity), validity_(capacity) {}

    void AllocateFlat();

    LogicalType type_;
    VectorFormat format_ = VectorFormat::Flat;
    idx_t capacity_;
    std::shared_ptr<Buffer> data_; // Flat: capacity*width bytes. Constant: >= width bytes.
    ValidityMask validity_;
    std::shared_ptr<StringHeap> heap_;
    std::shared_ptr<Vector> child_; // Dictionary only; always Flat
    SelectionVector sel_;           // Dictionary only
};

namespace VectorOps {

// Copies rows into the Flat vector `dst`:  dst[dst_offset + i] = src[sel ? sel[i] : i]
// for i in [0, count). `src` may be in any format; `sel` (if non-null) indexes src's logical rows.
// Out-of-line strings are copied into dst's heap, so dst is independent of src afterwards.
void Copy(const Vector& src, Vector& dst, const SelectionVector* sel, idx_t count,
          idx_t dst_offset = 0);

} // namespace VectorOps

} // namespace cdb
