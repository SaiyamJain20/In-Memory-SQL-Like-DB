#include "memory/arena.h"

#include "common/assert.h"
#include "common/types.h"

#include <algorithm>
#include <new>

namespace cdb {

namespace {
constexpr size_t kNoBlock = static_cast<size_t>(-1);
} // namespace

void Arena::BlockDeleter::Free(uint8_t* p) noexcept {
    ::operator delete(p, std::align_val_t{kMaxAlignment});
}

Arena::Arena(size_t block_size) : block_size_(std::max<size_t>(block_size, 64)) {}

Arena::~Arena() {
    FreeBlocks();
}

Arena::Arena(Arena&& other) noexcept
    : block_size_(other.block_size_), blocks_(std::move(other.blocks_)), active_(other.active_),
      offset_(other.offset_), used_(other.used_), reserved_(other.reserved_) {
    other.blocks_.clear();
    other.active_ = kNoBlock;
    other.offset_ = other.used_ = other.reserved_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        FreeBlocks();
        block_size_ = other.block_size_;
        blocks_ = std::move(other.blocks_);
        active_ = other.active_;
        offset_ = other.offset_;
        used_ = other.used_;
        reserved_ = other.reserved_;
        other.blocks_.clear();
        other.active_ = kNoBlock;
        other.offset_ = other.used_ = other.reserved_ = 0;
    }
    return *this;
}

void Arena::FreeBlocks() noexcept {
    for (const Block& b : blocks_) {
        BlockDeleter::Free(b.data);
    }
    blocks_.clear();
    active_ = kNoBlock;
    offset_ = used_ = reserved_ = 0;
}

Arena::Block Arena::NewBlock(size_t size) {
    auto* p = static_cast<uint8_t*>(::operator new(size, std::align_val_t{kMaxAlignment}));
    reserved_ += size;
    return Block{p, size};
}

void* Arena::Allocate(size_t size, size_t alignment) {
    CDB_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0 && alignment <= kMaxAlignment);

    // Fast path: bump within the active block.
    if (active_ != kNoBlock) {
        const Block& b = blocks_[active_];
        const size_t aligned = AlignUp(offset_, alignment);
        if (aligned + size <= b.size) {
            offset_ = aligned + size;
            used_ += size;
            return b.data + aligned;
        }
    }

    // Large allocations get a dedicated block so they do not waste the active block's tail.
    if (size > block_size_ / 4) {
        blocks_.push_back(NewBlock(std::max<size_t>(size, 1)));
        used_ += size;
        return blocks_.back().data; // blocks are kMaxAlignment-aligned
    }

    blocks_.push_back(NewBlock(block_size_));
    active_ = blocks_.size() - 1;
    offset_ = size;
    used_ += size;
    return blocks_[active_].data;
}

void Arena::Reset() {
    if (blocks_.empty()) {
        return;
    }
    // Keep the first block of regular size for reuse; free the rest.
    size_t keep = kNoBlock;
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i].size == block_size_) {
            keep = i;
            break;
        }
    }
    Block kept{nullptr, 0};
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (i == keep) {
            kept = blocks_[i];
        } else {
            BlockDeleter::Free(blocks_[i].data);
        }
    }
    blocks_.clear();
    reserved_ = 0;
    used_ = 0;
    offset_ = 0;
    active_ = kNoBlock;
    if (kept.data != nullptr) {
        blocks_.push_back(kept);
        reserved_ = kept.size;
        active_ = 0;
    }
}

} // namespace cdb
