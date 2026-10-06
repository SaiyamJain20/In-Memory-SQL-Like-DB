#pragma once

#include "common/error.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cdb {

static_assert(std::endian::native == std::endian::little,
              "the on-disk formats are little-endian and read by copying bytes");

// Appends fixed-width little-endian values to a byte buffer.
class BinaryWriter {
  public:
    void U8(uint8_t v) { buffer_.push_back(v); }
    void U32(uint32_t v) { Put(v); }
    void U64(uint64_t v) { Put(v); }
    void I64(int64_t v) { Put(v); }
    void F64(double v) { Put(v); } // the bit pattern, so NaN payloads and -0.0 survive
    void Bytes(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), p, p + n);
    }
    // u32 length, then the bytes.
    void String(std::string_view s) {
        U32(static_cast<uint32_t>(s.size()));
        Bytes(s.data(), s.size());
    }
    // Room for a u32 that is filled in later (a length known only at the end).
    size_t ReserveU32() {
        const size_t at = buffer_.size();
        U32(0);
        return at;
    }
    void PatchU32(size_t at, uint32_t v) { std::memcpy(buffer_.data() + at, &v, sizeof(v)); }

    size_t size() const noexcept { return buffer_.size(); }
    const std::vector<uint8_t>& buffer() const noexcept { return buffer_; }
    std::vector<uint8_t> Take() { return std::move(buffer_); }
    void Clear() { buffer_.clear(); }

  private:
    template <class T> void Put(T v) { Bytes(&v, sizeof(v)); }
    std::vector<uint8_t> buffer_;
};

// Reads what BinaryWriter wrote, from memory it does not own. Every read is bounds-checked: reading
// past the end, or a count that cannot fit in what is left, throws Error(Corruption) - a damaged or
// hostile file never makes the reader touch memory outside its buffer or allocate absurd amounts.
class BinaryReader {
  public:
    // `what` names the thing being read ("checkpoint footer") for error messages.
    BinaryReader(const uint8_t* data, size_t size, std::string what)
        : data_(data), size_(size), what_(std::move(what)) {}

    size_t position() const noexcept { return pos_; }
    size_t remaining() const noexcept { return size_ - pos_; }
    bool AtEnd() const noexcept { return pos_ == size_; }
    const std::string& what() const noexcept { return what_; }

    uint8_t U8() { return Get<uint8_t>(); }
    uint32_t U32() { return Get<uint32_t>(); }
    uint64_t U64() { return Get<uint64_t>(); }
    int64_t I64() { return Get<int64_t>(); }
    double F64() { return Get<double>(); }

    // A pointer to the next `n` bytes, which stay in the underlying buffer.
    const uint8_t* Bytes(size_t n) {
        Need(n);
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }
    std::string_view String() {
        const uint32_t n = U32();
        return {reinterpret_cast<const char*>(Bytes(n)), n};
    }
    // A u32 element count, rejected unless `count * min_bytes_each` can fit in the rest of the
    // buffer.
    uint32_t Count(size_t min_bytes_each) {
        const uint32_t n = U32();
        if (min_bytes_each > 0 && n > remaining() / min_bytes_each) {
            Fail("a count of " + std::to_string(n) + " cannot fit in the remaining " +
                 std::to_string(remaining()) + " bytes");
        }
        return n;
    }

    [[noreturn]] void Fail(const std::string& why) const {
        throw Error(ErrorCode::Corruption,
                    what_ + ": " + why + " (at byte " + std::to_string(pos_) + ")");
    }
    void ExpectEnd() const {
        if (pos_ != size_) {
            Fail(std::to_string(size_ - pos_) + " unexpected trailing bytes");
        }
    }

  private:
    void Need(size_t n) const {
        if (n > size_ - pos_) {
            Fail("truncated: " + std::to_string(n) + " more bytes needed, " +
                 std::to_string(size_ - pos_) + " left");
        }
    }
    template <class T> T Get() {
        Need(sizeof(T));
        T v;
        std::memcpy(&v, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    std::string what_;
};

} // namespace cdb
