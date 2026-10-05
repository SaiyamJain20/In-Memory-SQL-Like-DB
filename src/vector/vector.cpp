#include "vector/vector.h"

#include <cstring>
#include <utility>

namespace cdb {

namespace {

// Calls f(T{}) with T = the C++ element type of `type`.
template <class F> decltype(auto) DispatchPhysical(PhysicalType type, F&& f) {
    switch (type) {
    case PhysicalType::Bool:
        return f(bool{});
    case PhysicalType::Int32:
        return f(int32_t{});
    case PhysicalType::Int64:
        return f(int64_t{});
    case PhysicalType::Double:
        return f(double{});
    case PhysicalType::String:
        return f(string_t{});
    }
    CDB_UNREACHABLE("DispatchPhysical");
}

// Reads element `idx` of raw storage as a Value of logical type `type`.
Value ReadValue(LogicalType type, const uint8_t* data, idx_t idx) {
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(reinterpret_cast<const bool*>(data)[idx]);
    case TypeId::Integer:
        return Value::Integer(reinterpret_cast<const int32_t*>(data)[idx]);
    case TypeId::BigInt:
        return Value::BigInt(reinterpret_cast<const int64_t*>(data)[idx]);
    case TypeId::Double:
        return Value::Double(reinterpret_cast<const double*>(data)[idx]);
    case TypeId::Date:
        return Value::Date(date_t{reinterpret_cast<const int32_t*>(data)[idx]});
    case TypeId::Varchar:
        return Value::Varchar(std::string(reinterpret_cast<const string_t*>(data)[idx].view()));
    }
    CDB_UNREACHABLE("ReadValue");
}

} // namespace

Vector::Vector(LogicalType type, idx_t capacity)
    : type_(type), capacity_(capacity), validity_(capacity) {
    CDB_CHECK(capacity >= 1 && capacity <= kVectorSize);
    AllocateFlat();
}

void Vector::AllocateFlat() {
    data_ = Buffer::Allocate(capacity_ * type_.width());
}

Vector Vector::MakeConstant(const Value& value, idx_t capacity) {
    Vector v(value.type(), capacity);
    v.SetConstant(value);
    return v;
}

StringHeap& Vector::Heap() {
    CDB_ASSERT(type_.id() == TypeId::Varchar &&
               (format_ == VectorFormat::Flat || format_ == VectorFormat::Constant));
    if (!heap_) {
        heap_ = std::make_shared<StringHeap>();
    }
    return *heap_;
}

Value Vector::GetValue(idx_t row) const {
    CDB_CHECK(row < capacity_);
    if (format_ == VectorFormat::Dictionary) {
        CDB_CHECK(row < sel_.capacity());
    }
    UnifiedFormat u;
    ToUnified(u);
    const idx_t idx = u.sel[row];
    if (!u.validity->IsValid(idx)) {
        return Value::Null(type_);
    }
    return ReadValue(type_, u.data, idx);
}

void Vector::SetValue(idx_t row, const Value& value) {
    CDB_CHECK(format_ == VectorFormat::Flat && row < capacity_);
    CDB_CHECK(value.type() == type_);
    if (value.IsNull()) {
        validity_.SetInvalid(row);
        return;
    }
    validity_.SetValid(row);
    switch (type_.id()) {
    case TypeId::Boolean:
        data_->As<bool>()[row] = value.GetBoolean();
        break;
    case TypeId::Integer:
        data_->As<int32_t>()[row] = value.GetInteger();
        break;
    case TypeId::BigInt:
        data_->As<int64_t>()[row] = value.GetBigInt();
        break;
    case TypeId::Double:
        data_->As<double>()[row] = value.GetDouble();
        break;
    case TypeId::Date:
        data_->As<int32_t>()[row] = value.GetDate().days;
        break;
    case TypeId::Varchar:
        data_->As<string_t>()[row] = AddString(value.GetVarchar());
        break;
    }
}

void Vector::Reset() {
    format_ = VectorFormat::Flat;
    child_.reset();
    sel_ = SelectionVector();
    validity_.Reset(capacity_);
    const size_t needed = capacity_ * type_.width();
    if (!data_ || data_.use_count() > 1 || data_->size() < needed) {
        AllocateFlat();
    }
    if (heap_) {
        if (heap_.use_count() == 1) {
            heap_->Reset();
        } else {
            heap_.reset();
        }
    }
}

void Vector::Reference(const Vector& other) {
    type_ = other.type_;
    format_ = other.format_;
    capacity_ = other.capacity_;
    data_ = other.data_;
    validity_ = other.validity_; // shallow by design
    heap_ = other.heap_;
    child_ = other.child_;
    sel_ = other.sel_;
}

void Vector::SetConstant(const Value& value) {
    CDB_CHECK(value.type() == type_);
    child_.reset();
    sel_ = SelectionVector();
    format_ = VectorFormat::Constant;
    validity_.Reset(capacity_);
    const size_t width = type_.width();
    if (!data_ || data_.use_count() > 1 || data_->size() < width) {
        data_ = Buffer::Allocate(std::max<size_t>(width, 16));
    }
    if (heap_) {
        if (heap_.use_count() == 1) {
            heap_->Reset();
        } else {
            heap_.reset();
        }
    }
    if (value.IsNull()) {
        validity_.SetInvalid(0);
        return;
    }
    switch (type_.id()) {
    case TypeId::Boolean:
        data_->As<bool>()[0] = value.GetBoolean();
        break;
    case TypeId::Integer:
        data_->As<int32_t>()[0] = value.GetInteger();
        break;
    case TypeId::BigInt:
        data_->As<int64_t>()[0] = value.GetBigInt();
        break;
    case TypeId::Double:
        data_->As<double>()[0] = value.GetDouble();
        break;
    case TypeId::Date:
        data_->As<int32_t>()[0] = value.GetDate().days;
        break;
    case TypeId::Varchar:
        data_->As<string_t>()[0] = AddString(value.GetVarchar());
        break;
    }
}

void Vector::Flatten(idx_t count) {
    CDB_CHECK(count <= capacity_);
    if (format_ == VectorFormat::Flat) {
        return;
    }
    UnifiedFormat u;
    ToUnified(u);

    auto new_data = Buffer::Allocate(capacity_ * type_.width());
    ValidityMask new_validity(capacity_);
    DispatchPhysical(type_.physical(), [&](auto tag) {
        using T = decltype(tag);
        const T* src = u.Data<T>();
        T* dst = new_data->As<T>();
        for (idx_t i = 0; i < count; i++) {
            const sel_t s = u.sel[i];
            if (u.validity->IsValid(s)) {
                dst[i] = src[s];
            } else {
                new_validity.SetInvalid(i);
            }
        }
    });

    // Out-of-line strings keep pointing into the child's heap; share it so they stay alive.
    if (format_ == VectorFormat::Dictionary) {
        heap_ = child_->heap_;
    }
    data_ = std::move(new_data);
    validity_ = std::move(new_validity);
    child_.reset();
    sel_ = SelectionVector();
    format_ = VectorFormat::Flat;
}

void Vector::Slice(const SelectionVector& sel, idx_t count) {
    CDB_CHECK(count <= capacity_);
    if (format_ == VectorFormat::Constant) {
        return;
    }
    SelectionVector new_sel(count);
    if (format_ == VectorFormat::Flat) {
        for (idx_t i = 0; i < count; i++) {
            new_sel.Set(i, sel[i]);
        }
        auto child = std::shared_ptr<Vector>(new Vector(type_, capacity_, NoAllocTag{}));
        child->format_ = VectorFormat::Flat;
        child->data_ = std::move(data_);
        child->validity_ = std::move(validity_);
        child->heap_ = std::move(heap_);
        child_ = std::move(child);
        data_.reset();
        validity_ = ValidityMask(capacity_);
        heap_.reset();
        format_ = VectorFormat::Dictionary;
    } else {
        for (idx_t i = 0; i < count; i++) {
            new_sel.Set(i, sel_[sel[i]]);
        }
    }
    sel_ = std::move(new_sel);
}

void Vector::ToUnified(UnifiedFormat& out) const {
    switch (format_) {
    case VectorFormat::Flat:
        out.sel = SelectionVector::Identity().data();
        out.data = data_->data();
        out.validity = &validity_;
        return;
    case VectorFormat::Constant:
        out.sel = SelectionVector::Zeros().data();
        out.data = data_->data();
        out.validity = &validity_;
        return;
    case VectorFormat::Dictionary:
        out.sel = sel_.data();
        out.data = child_->data_->data();
        out.validity = &child_->validity_;
        return;
    }
    CDB_UNREACHABLE("Vector::ToUnified");
}

void Vector::Verify(idx_t count) const {
    CDB_CHECK(count <= capacity_);
    CDB_CHECK(capacity_ >= 1 && capacity_ <= kVectorSize);
    const Vector* flat = this;
    idx_t rows = count;
    switch (format_) {
    case VectorFormat::Flat:
        CDB_CHECK(data_ != nullptr && data_->size() >= capacity_ * type_.width());
        CDB_CHECK(child_ == nullptr);
        break;
    case VectorFormat::Constant:
        CDB_CHECK(data_ != nullptr && data_->size() >= type_.width());
        rows = count == 0 ? 0 : 1;
        break;
    case VectorFormat::Dictionary: {
        CDB_CHECK(child_ != nullptr && child_->format_ == VectorFormat::Flat);
        CDB_CHECK(child_->type_ == type_);
        CDB_CHECK(sel_.IsSet() && sel_.capacity() >= count);
        for (idx_t i = 0; i < count; i++) {
            CDB_CHECK(sel_[i] < child_->capacity_);
        }
        flat = child_.get();
        // Strings referenced through the dictionary must be well-formed: check the whole child.
        rows = flat->capacity_;
        break;
    }
    }
    if (type_.physical() == PhysicalType::String) {
        const auto* strs = flat->data_->As<string_t>();
        for (idx_t i = 0; i < rows; i++) {
            if (!flat->validity_.IsValid(i)) {
                continue;
            }
            const string_t& s = strs[i];
            if (s.IsInlined()) {
                // Zero-padding invariant: bytes past the length must be zero.
                const auto* raw = reinterpret_cast<const uint8_t*>(&s);
                for (size_t b = 4 + s.size(); b < sizeof(string_t); b++) {
                    CDB_CHECK(raw[b] == 0);
                }
            } else {
                CDB_CHECK(s.data() != nullptr);
            }
        }
    }
}

namespace VectorOps {

void Copy(const Vector& src, Vector& dst, const SelectionVector* sel, idx_t count,
          idx_t dst_offset) {
    CDB_CHECK(src.type() == dst.type());
    CDB_CHECK(dst.format() == VectorFormat::Flat);
    CDB_CHECK(dst_offset + count <= dst.capacity());

    UnifiedFormat u;
    src.ToUnified(u);
    ValidityMask& dst_validity = dst.Validity();
    auto source_slot = [&](idx_t i) -> sel_t {
        const idx_t logical = sel != nullptr ? (*sel)[i] : i;
        CDB_ASSERT(logical < src.capacity());
        return u.sel[logical];
    };

    // 1) Values. Fixed-width types copy every row without looking at validity (what is stored
    //    in a NULL slot is irrelevant), which keeps the loop branch-free; a flat source with no
    //    selection is a plain memcpy. Strings must skip NULL slots so that no stale out-of-line
    //    pointer is ever dereferenced.
    DispatchPhysical(src.type().physical(), [&](auto tag) {
        using T = decltype(tag);
        const T* in = u.Data<T>();
        T* out = dst.template FlatData<T>() + dst_offset;
        if constexpr (std::is_same_v<T, string_t>) {
            for (idx_t i = 0; i < count; i++) {
                const sel_t s = source_slot(i);
                if (!u.validity->IsValid(s)) {
                    out[i] = string_t();
                } else {
                    out[i] = in[s].IsInlined() ? in[s] : dst.AddString(in[s].view());
                }
            }
        } else if (sel == nullptr && src.format() == VectorFormat::Flat) {
            if (count > 0) {
                std::memcpy(out, in, count * sizeof(T));
            }
        } else {
            for (idx_t i = 0; i < count; i++) {
                out[i] = in[source_slot(i)];
            }
        }
    });

    // 2) Validity. A fully valid source only needs the destination range marked valid (a
    //    word-wise operation, free if the destination has never held a NULL).
    if (u.validity->AllValid()) {
        dst_validity.SetRangeValid(dst_offset, count);
    } else {
        for (idx_t i = 0; i < count; i++) {
            dst_validity.Set(dst_offset + i, u.validity->IsValid(source_slot(i)));
        }
    }
}

} // namespace VectorOps

} // namespace cdb
