#pragma once

#include "common/assert.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace cdb {

// 16-byte string view in the "German string" layout:
//
//   inlined (length <= 12):  | length:u32 | bytes[12] zero-padded            |
//   out-of-line (> 12):      | length:u32 | prefix[4] | const char* pointer  |
//
// Short strings need no heap at all, and the 4-byte prefix lets most comparisons resolve without
// dereferencing the pointer. Invariant: for inlined strings every unused byte is zero, so two
// inlined strings are equal iff their 16 bytes are equal.
//
// string_t does NOT own out-of-line bytes: they live in a StringHeap (or storage segment) that
// must outlive every string_t pointing into it.
//
// Lifetime hazard: for an INLINED string, data()/view() point into the string_t object itself, so
// they dangle if that particular object (e.g. a local copy) is destroyed. Take views from the
// string_t stored in its vector, not from a temporary copy.
class string_t {
  public:
    static constexpr uint32_t kInlineCapacity = 12;
    static constexpr uint32_t kPrefixLength = 4;

    string_t() noexcept { std::memset(bytes_, 0, sizeof(bytes_)); }

    // Constructs an inlined string. Requires len <= kInlineCapacity.
    static string_t MakeInlined(const char* data, uint32_t len) noexcept {
        CDB_ASSERT(len <= kInlineCapacity);
        string_t s;
        std::memcpy(s.bytes_, &len, sizeof(len));
        if (len > 0) {
            std::memcpy(s.bytes_ + 4, data, len);
        }
        return s;
    }

    // Constructs an out-of-line string referring to `data` (not copied!). Requires
    // len > kInlineCapacity.
    static string_t MakeReference(const char* data, uint32_t len) noexcept {
        CDB_ASSERT(len > kInlineCapacity);
        string_t s;
        std::memcpy(s.bytes_, &len, sizeof(len));
        std::memcpy(s.bytes_ + 4, data, kPrefixLength);
        std::memcpy(s.bytes_ + 8, &data, sizeof(data));
        return s;
    }

    // Convenience: inlines when it fits, otherwise references `data` (caller keeps it alive).
    static string_t FromView(std::string_view v) noexcept {
        const auto len = static_cast<uint32_t>(v.size());
        return len <= kInlineCapacity ? MakeInlined(v.data(), len) : MakeReference(v.data(), len);
    }

    uint32_t size() const noexcept {
        uint32_t len;
        std::memcpy(&len, bytes_, sizeof(len));
        return len;
    }
    bool empty() const noexcept { return size() == 0; }
    bool IsInlined() const noexcept { return size() <= kInlineCapacity; }

    const char* data() const noexcept {
        if (IsInlined()) {
            return reinterpret_cast<const char*>(bytes_ + 4);
        }
        const char* p;
        std::memcpy(&p, bytes_ + 8, sizeof(p));
        return p;
    }

    std::string_view view() const noexcept { return std::string_view(data(), size()); }

    // Bytes 4..8 of the representation: the first 4 characters (zero-padded if shorter).
    uint32_t Prefix() const noexcept {
        uint32_t p;
        std::memcpy(&p, bytes_ + 4, sizeof(p));
        return p;
    }

    friend bool operator==(const string_t& a, const string_t& b) noexcept {
        // length + prefix in one 8-byte compare
        uint64_t ha, hb;
        std::memcpy(&ha, a.bytes_, 8);
        std::memcpy(&hb, b.bytes_, 8);
        if (ha != hb) {
            return false;
        }
        if (a.IsInlined()) {
            uint64_t ta, tb; // remaining inline bytes (zero padded by invariant)
            std::memcpy(&ta, a.bytes_ + 8, 8);
            std::memcpy(&tb, b.bytes_ + 8, 8);
            return ta == tb;
        }
        return std::memcmp(a.data() + kPrefixLength, b.data() + kPrefixLength,
                           a.size() - kPrefixLength) == 0;
    }
    friend bool operator!=(const string_t& a, const string_t& b) noexcept { return !(a == b); }

    // Three-way unsigned-bytewise lexicographic comparison (<0, 0, >0), identical in outcome to
    // std::string_view::compare for every input.
    static int Compare(const string_t& a, const string_t& b) noexcept {
        // Big-endian prefix compare == lexicographic compare of the first 4 bytes. Zero padding
        // of short strings keeps this correct: a shorter string that is a prefix of a longer one
        // sorts first or ties (and ties fall through to the full compare below).
        const uint32_t pa = ByteSwap(a.Prefix());
        const uint32_t pb = ByteSwap(b.Prefix());
        if (pa != pb) {
            return pa < pb ? -1 : 1;
        }
        const uint32_t la = a.size();
        const uint32_t lb = b.size();
        const uint32_t min_len = la < lb ? la : lb;
        if (min_len > kPrefixLength) {
            const int c = std::memcmp(a.data() + kPrefixLength, b.data() + kPrefixLength,
                                      min_len - kPrefixLength);
            if (c != 0) {
                return c < 0 ? -1 : 1;
            }
        }
        return la < lb ? -1 : (la > lb ? 1 : 0);
    }

    friend bool operator<(const string_t& a, const string_t& b) noexcept {
        return Compare(a, b) < 0;
    }
    friend bool operator<=(const string_t& a, const string_t& b) noexcept {
        return Compare(a, b) <= 0;
    }
    friend bool operator>(const string_t& a, const string_t& b) noexcept {
        return Compare(a, b) > 0;
    }
    friend bool operator>=(const string_t& a, const string_t& b) noexcept {
        return Compare(a, b) >= 0;
    }

  private:
    static uint32_t ByteSwap(uint32_t v) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return __builtin_bswap32(v);
        } else {
            return v;
        }
    }

    alignas(8) uint8_t bytes_[16];
};

static_assert(sizeof(string_t) == 16);
static_assert(alignof(string_t) == 8);

} // namespace cdb
