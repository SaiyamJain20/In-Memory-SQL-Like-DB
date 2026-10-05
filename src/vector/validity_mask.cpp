#include "vector/validity_mask.h"

#include "common/assert.h"

#include <bit>
#include <cstring>

namespace cdb {

uint64_t* ValidityMask::MutableWords() {
    if (buffer_ == nullptr) {
        const idx_t words = WordCount(capacity_);
        buffer_ = Buffer::Allocate(words * sizeof(uint64_t));
        std::memset(buffer_->data(), 0xFF, words * sizeof(uint64_t));
    }
    return buffer_->As<uint64_t>();
}

void ValidityMask::SetInvalid(idx_t row) {
    CDB_ASSERT(row < capacity_);
    MutableWords()[row >> 6] &= ~(uint64_t{1} << (row & 63));
}

void ValidityMask::SetValid(idx_t row) {
    CDB_ASSERT(row < capacity_);
    if (buffer_ == nullptr) {
        return;
    }
    buffer_->As<uint64_t>()[row >> 6] |= uint64_t{1} << (row & 63);
}

void ValidityMask::SetRangeValid(idx_t start, idx_t count) {
    CDB_ASSERT(start + count <= capacity_);
    if (buffer_ == nullptr || count == 0) {
        return;
    }
    uint64_t* words = buffer_->As<uint64_t>();
    const idx_t end = start + count; // exclusive
    const idx_t first_word = start >> 6;
    const idx_t last_word = (end - 1) >> 6;
    const uint64_t first_mask = ~uint64_t{0} << (start & 63);
    const uint64_t last_mask = (end & 63) == 0 ? ~uint64_t{0} : (uint64_t{1} << (end & 63)) - 1;
    if (first_word == last_word) {
        words[first_word] |= first_mask & last_mask;
        return;
    }
    words[first_word] |= first_mask;
    for (idx_t w = first_word + 1; w < last_word; w++) {
        words[w] = ~uint64_t{0};
    }
    words[last_word] |= last_mask;
}

void ValidityMask::SetAllInvalid(idx_t count) {
    CDB_ASSERT(count <= capacity_);
    uint64_t* words = MutableWords();
    const idx_t full = count >> 6;
    for (idx_t w = 0; w < full; w++) {
        words[w] = 0;
    }
    if ((count & 63) != 0) {
        words[full] &= ~((uint64_t{1} << (count & 63)) - 1);
    }
}

idx_t ValidityMask::CountValid(idx_t count) const noexcept {
    if (buffer_ == nullptr) {
        return count;
    }
    const uint64_t* words = Words();
    idx_t valid = 0;
    const idx_t full = count >> 6;
    for (idx_t w = 0; w < full; w++) {
        valid += static_cast<idx_t>(std::popcount(words[w]));
    }
    if ((count & 63) != 0) {
        const uint64_t tail_mask = (uint64_t{1} << (count & 63)) - 1;
        valid += static_cast<idx_t>(std::popcount(words[full] & tail_mask));
    }
    return valid;
}

ValidityMask ValidityMask::DeepCopy() const {
    ValidityMask copy(capacity_);
    if (buffer_ != nullptr) {
        copy.buffer_ = Buffer::Allocate(buffer_->size());
        std::memcpy(copy.buffer_->data(), buffer_->data(), buffer_->size());
    }
    return copy;
}

} // namespace cdb
