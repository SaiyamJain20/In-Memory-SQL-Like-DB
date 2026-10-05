#pragma once

#include "memory/arena.h"
#include "types/string_t.h"

#include <string_view>

namespace cdb {

// Append-only storage for the bytes of out-of-line strings (> 12 bytes). Addresses are stable, so
// string_t values pointing into a heap stay valid until the heap is Reset() or destroyed. Vectors
// share heaps (via shared_ptr) when they reference each other's strings.
class StringHeap {
  public:
    // Returns a string_t holding a copy of `value`: inlined when short, otherwise copied into
    // the arena.
    string_t Add(std::string_view value);

    void Reset() { arena_.Reset(); }
    size_t BytesUsed() const noexcept { return arena_.BytesUsed(); }

  private:
    Arena arena_;
};

} // namespace cdb
