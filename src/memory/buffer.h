#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cdb {

// A fixed-size, zero-initialised, 64-byte-aligned heap block with reference-counted ownership.
//
// Every Buffer has at least kPadding bytes of readable and writable slack after size(), so SIMD
// kernels may load/store a whole register at a vector's tail without running off the allocation
// (this keeps AddressSanitizer quiet about intentional tail over-reads).
class Buffer {
  public:
    static constexpr size_t kAlignment = 64;
    static constexpr size_t kPadding = 64;

    static std::shared_ptr<Buffer> Allocate(size_t bytes);

    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }

    template <class T> T* As() noexcept { return reinterpret_cast<T*>(data_); }
    template <class T> const T* As() const noexcept { return reinterpret_cast<const T*>(data_); }

  private:
    explicit Buffer(size_t bytes);

    uint8_t* data_;
    size_t size_;
};

} // namespace cdb
