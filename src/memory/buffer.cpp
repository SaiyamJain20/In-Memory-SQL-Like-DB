#include "memory/buffer.h"

#include "common/assert.h"
#include "common/types.h"

#include <cstring>
#include <new>

namespace cdb {

Buffer::Buffer(size_t bytes) : size_(bytes) {
    const size_t total = AlignUp(bytes, kAlignment) + kPadding;
    data_ = static_cast<uint8_t*>(::operator new(total, std::align_val_t{kAlignment}));
    std::memset(data_, 0, total);
}

Buffer::Buffer(std::shared_ptr<Buffer> parent, size_t offset, size_t size)
    : data_(parent->data_ + offset), size_(size), parent_(std::move(parent)) {}

Buffer::~Buffer() {
    if (parent_ == nullptr) {
        ::operator delete(data_, std::align_val_t{kAlignment});
    }
}

std::shared_ptr<Buffer> Buffer::View(std::shared_ptr<Buffer> parent, size_t offset, size_t size) {
    CDB_CHECK(parent != nullptr && offset + size <= parent->size());
    return std::shared_ptr<Buffer>(new Buffer(std::move(parent), offset, size));
}

std::shared_ptr<Buffer> Buffer::Allocate(size_t bytes) {
    return std::shared_ptr<Buffer>(new Buffer(bytes));
}

} // namespace cdb
