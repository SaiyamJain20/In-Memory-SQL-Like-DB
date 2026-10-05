#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cdb {

// A fixed-size, zero-initialised, 64-byte-aligned heap block with reference-counted ownership.
//
// Every owning Buffer has at least kPadding bytes of readable and writable slack after size(), so
// SIMD kernels may load a whole register at a vector's tail without running off the allocation
// (this keeps AddressSanitizer quiet about intentional tail over-reads). A View is a read-only
// window into another Buffer; the bytes after it are always readable too (they are the parent's
// next bytes or its padding), but must never be written.
class Buffer {
  public:
    static constexpr size_t kAlignment = 64;
    static constexpr size_t kPadding = 64;

    static std::shared_ptr<Buffer> Allocate(size_t bytes);

    // A zero-copy, READ-ONLY window [offset, offset + size) into `parent`, which it keeps alive.
    // Requires offset + size <= parent->size().
    static std::shared_ptr<Buffer> View(std::shared_ptr<Buffer> parent, size_t offset, size_t size);

    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    // True for Views: the bytes belong to an immutable owner and must not be modified.
    bool read_only() const noexcept { return parent_ != nullptr; }

    template <class T> T* As() noexcept { return reinterpret_cast<T*>(data_); }
    template <class T> const T* As() const noexcept { return reinterpret_cast<const T*>(data_); }

  private:
    explicit Buffer(size_t bytes);
    Buffer(std::shared_ptr<Buffer> parent, size_t offset, size_t size);

    uint8_t* data_;
    size_t size_;
    std::shared_ptr<Buffer> parent_; // non-null iff this is a View
};

} // namespace cdb
