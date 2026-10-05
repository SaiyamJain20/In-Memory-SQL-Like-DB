#include "storage/column_segment.h"

namespace cdb {

ColumnSegment::ColumnSegment(LogicalType type, idx_t count, std::shared_ptr<Buffer> data,
                             ValidityMask validity, std::shared_ptr<StringHeap> heap,
                             ColumnStats stats)
    : type_(type), count_(count), data_(std::move(data)), validity_(std::move(validity)),
      heap_(std::move(heap)), stats_(std::move(stats)) {
    const idx_t vectors = (count_ + kVectorSize - 1) / kVectorSize;
    CDB_CHECK(data_ != nullptr && data_->size() >= vectors * kVectorSize * type_.width());
    CDB_CHECK(stats_.count == count_);
    constexpr size_t kWordsPerVector = kVectorSize / 64;
    CDB_CHECK(validity_.AllValid() ||
              validity_.buffer()->size() >= vectors * kWordsPerVector * sizeof(uint64_t));
    for (idx_t v = 0; v < vectors; v++) {
        data_views_.push_back(
            Buffer::View(data_, v * kVectorSize * type_.width(), kVectorSize * type_.width()));
        if (!validity_.AllValid()) {
            validity_views_.push_back(Buffer::View(validity_.buffer(),
                                                   v * kWordsPerVector * sizeof(uint64_t),
                                                   kWordsPerVector * sizeof(uint64_t)));
        }
    }
}

void ColumnSegment::Scan(idx_t offset, idx_t n, Vector& out) const {
    CDB_CHECK(offset % kVectorSize == 0 && n <= kVectorSize && offset + n <= count_);
    CDB_CHECK(out.type() == type_ && out.capacity() == kVectorSize);
    scan_calls_.fetch_add(1, std::memory_order_relaxed);
    const idx_t v = offset / kVectorSize;
    ValidityMask validity = validity_.AllValid()
                                ? ValidityMask(kVectorSize)
                                : ValidityMask::FromBuffer(validity_views_[v], kVectorSize);
    out.ReferenceFlat(data_views_[v], std::move(validity), heap_);
}

size_t ColumnSegment::MemoryUsage() const noexcept {
    size_t bytes = data_->size();
    if (!validity_.AllValid()) {
        bytes += validity_.buffer()->size();
    }
    if (heap_ != nullptr) {
        bytes += heap_->BytesUsed();
    }
    return bytes;
}

} // namespace cdb
