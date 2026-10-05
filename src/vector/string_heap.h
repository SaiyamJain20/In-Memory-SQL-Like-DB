#pragma once

#include "common/assert.h"
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

    // After Seal() the heap is immutable: Add() of an out-of-line string aborts. Storage segments
    // seal their heaps so scans can share them across threads without synchronisation.
    void Seal() noexcept { sealed_ = true; }
    bool sealed() const noexcept { return sealed_; }

    void Reset() {
        CDB_CHECK(!sealed_);
        arena_.Reset();
    }
    size_t BytesUsed() const noexcept { return arena_.BytesUsed(); }

  private:
    Arena arena_;
    bool sealed_ = false;
};

} // namespace cdb
