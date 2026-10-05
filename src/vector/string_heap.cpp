#include "vector/string_heap.h"

#include "common/assert.h"

#include <cstring>
#include <limits>

namespace cdb {

string_t StringHeap::Add(std::string_view value) {
    CDB_CHECK(value.size() <= std::numeric_limits<uint32_t>::max());
    const auto len = static_cast<uint32_t>(value.size());
    if (len <= string_t::kInlineCapacity) {
        return string_t::MakeInlined(value.data(), len);
    }
    char* dst = static_cast<char*>(arena_.Allocate(len, 1));
    std::memcpy(dst, value.data(), len);
    return string_t::MakeReference(dst, len);
}

} // namespace cdb
