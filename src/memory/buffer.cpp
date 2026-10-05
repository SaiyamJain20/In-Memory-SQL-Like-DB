#include "memory/buffer.h"

#include "common/types.h"

#include <cstring>
#include <new>

namespace cdb {

Buffer::Buffer(size_t bytes) : size_(bytes) {
    const size_t total = AlignUp(bytes, kAlignment) + kPadding;
    data_ = static_cast<uint8_t*>(::operator new(total, std::align_val_t{kAlignment}));
    std::memset(data_, 0, total);
}

Buffer::~Buffer() {
    ::operator delete(data_, std::align_val_t{kAlignment});
}

std::shared_ptr<Buffer> Buffer::Allocate(size_t bytes) {
    return std::shared_ptr<Buffer>(new Buffer(bytes));
}

} // namespace cdb
