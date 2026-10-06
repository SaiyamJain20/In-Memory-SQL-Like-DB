#include "storage/column_builder.h"

#include "storage/encoding.h"

#include <algorithm>
#include <cstring>

namespace cdb {

ColumnBuilder::ColumnBuilder(LogicalType type, idx_t max_rows)
    : type_(type), max_rows_(max_rows), validity_(0) {
    CDB_CHECK(max_rows >= kVectorSize && max_rows % kVectorSize == 0);
    if (type_.id() == TypeId::Varchar) {
        heap_ = std::make_shared<StringHeap>();
    }
}

void ColumnBuilder::Reserve(idx_t rows) {
    CDB_CHECK(rows <= max_rows_);
    if (rows <= capacity_) {
        return;
    }
    idx_t new_capacity = capacity_ == 0 ? kVectorSize : capacity_;
    while (new_capacity < rows) {
        new_capacity *= 2;
    }
    new_capacity = std::min(new_capacity, max_rows_);
    auto grown = Buffer::Allocate(new_capacity * type_.width());
    if (count_ > 0) {
        std::memcpy(grown->data(), data_->data(), count_ * type_.width());
    }
    data_ = std::move(grown);
    validity_.Resize(new_capacity);
    capacity_ = new_capacity;
}

void ColumnBuilder::Append(const Vector& src, idx_t src_offset, idx_t n) {
    CDB_CHECK(src.type() == type_);
    CDB_CHECK(count_ + n <= max_rows_);
    if (n == 0) {
        return;
    }
    Reserve(count_ + n);
    VectorOps::CopyRows(src, nullptr, src_offset, n, data_->data(), validity_, heap_.get(), count_);
    count_ += n;
}

std::shared_ptr<ColumnSegment> ColumnBuilder::Snapshot() const {
    return SnapshotSegment(/*with_distinct_sketch=*/false);
}

std::shared_ptr<ColumnSegment> ColumnBuilder::SnapshotSegment(bool with_distinct_sketch) const {
    const idx_t rounded = AlignUp(count_, kVectorSize);
    auto data = Buffer::Allocate(rounded * type_.width());
    if (count_ > 0) {
        std::memcpy(data->data(), data_->data(), count_ * type_.width());
    }
    ValidityMask validity(rounded);
    if (!validity_.AllValid()) {
        const size_t bytes = ValidityMask::WordCount(rounded) * sizeof(uint64_t);
        auto words = Buffer::Allocate(bytes);
        std::memcpy(words->data(), validity_.Words(), std::min(bytes, validity_.buffer()->size()));
        validity = ValidityMask::FromBuffer(std::move(words), rounded);
    }
    ColumnStats stats = ComputeColumnStats(type_, data->data(), validity, count_);
    if (with_distinct_sketch) {
        stats.distinct = ComputeDistinctSketch(type_, data->data(), validity, count_);
    }
    return std::make_shared<ColumnSegment>(type_, count_, std::move(data), std::move(validity),
                                           heap_, std::move(stats));
}

std::shared_ptr<ColumnSegment> ColumnBuilder::Seal() {
    const idx_t rounded = AlignUp(count_, kVectorSize);
    std::shared_ptr<ColumnSegment> segment;
    if (count_ > 0 && capacity_ == rounded) {
        // Common case (a full row group): hand the buffers over without copying.
        ColumnStats stats = ComputeColumnStats(type_, data_->data(), validity_, count_);
        stats.distinct = ComputeDistinctSketch(type_, data_->data(), validity_, count_);
        segment = std::make_shared<ColumnSegment>(type_, count_, std::move(data_),
                                                  std::move(validity_), heap_, std::move(stats));
    } else {
        segment = SnapshotSegment(/*with_distinct_sketch=*/true);
    }
    if (heap_ != nullptr) {
        heap_->Seal();
    }
    segment = CompressSegment(std::move(segment)); // sealed segments are immutable: encode them
    data_.reset();
    validity_ = ValidityMask(0);
    heap_ = type_.id() == TypeId::Varchar ? std::make_shared<StringHeap>() : nullptr;
    capacity_ = 0;
    count_ = 0;
    return segment;
}

} // namespace cdb
