#include "execution/chunk_store.h"

#include "execution/type_dispatch.h"

#include <algorithm>

namespace cdb {

ChunkStore::ChunkStore(std::vector<LogicalType> types) : types_(std::move(types)) {}

DataChunk& ChunkStore::TailWithRoom() {
    if (chunks_.empty() || chunks_.back().size() == kVectorSize) {
        chunks_.emplace_back();
        chunks_.back().Initialize(types_);
    }
    return chunks_.back();
}

void ChunkStore::Append(const DataChunk& chunk, const SelectionVector* sel, idx_t count) {
    CDB_CHECK(chunk.types() == types_);
    idx_t done = 0;
    while (done < count) {
        DataChunk& tail = TailWithRoom();
        const idx_t at = tail.size();
        const idx_t take = std::min(count - done, kVectorSize - at);
        for (idx_t c = 0; c < types_.size(); c++) {
            Vector& dst = tail.column(c);
            VectorOps::CopyRows(
                chunk.column(c), sel, done, take, dst.FlatBytes(), dst.Validity(),
                types_[c].physical() == PhysicalType::String ? &dst.Heap() : nullptr, at);
        }
        tail.SetCardinality(at + take);
        done += take;
        count_ += take;
    }
}

void ChunkStore::AppendStore(const ChunkStore& other) {
    CDB_CHECK(other.types_ == types_);
    for (const DataChunk& c : other.chunks_) {
        Append(c, nullptr, c.size());
    }
}

void ChunkStore::Gather(idx_t column, const uint32_t* rows, idx_t n, Vector& out) const {
    CDB_ASSERT(column < types_.size() && out.format() == VectorFormat::Flat && out.capacity() >= n);
    DispatchPhysical(types_[column].physical(), [&](auto tag) {
        using T = decltype(tag);
        T* dst = out.FlatDataForOverwrite<T>();
        ValidityMask& dv = out.Validity();
        for (idx_t i = 0; i < n; i++) {
            const idx_t row = rows[i];
            const Vector& src = chunks_[row >> kShift].column(column);
            const idx_t off = row & (kVectorSize - 1);
            if (!src.Validity().IsValid(off)) {
                dv.SetInvalid(i);
                continue;
            }
            const T& v = src.FlatData<T>()[off];
            if constexpr (std::is_same_v<T, string_t>) {
                dst[i] = out.AddString(v.view());
            } else {
                dst[i] = v;
            }
        }
    });
}

void ChunkStore::ReadRange(idx_t first, idx_t n, DataChunk& out) const {
    CDB_CHECK(first + n <= count_ && n <= kVectorSize);
    std::vector<uint32_t> rows(n);
    for (idx_t i = 0; i < n; i++) {
        rows[i] = static_cast<uint32_t>(first + i);
    }
    out.Reset();
    for (idx_t c = 0; c < types_.size(); c++) {
        Gather(c, rows.data(), n, out.column(c));
    }
    out.SetCardinality(n);
}

void ChunkStore::Clear() {
    chunks_.clear();
    count_ = 0;
}

} // namespace cdb
