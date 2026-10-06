#include "execution/hashing.h"

#include "execution/type_dispatch.h"
#include "kernels/hash.h"

#include <cmath>
#include <cstring>

namespace cdb {

namespace {
constexpr uint64_t kNullHash = 0x6a09e667f3bcc909ULL;
constexpr uint64_t kNaNHash = 0xbb67ae8584caa73bULL;

// A short string (<= 12 bytes) is stored inline in the 16 bytes of its string_t, zero padded, so
// those 16 bytes identify it: hash the two words instead of walking the bytes. Longer strings hash
// by content. A given string is always inline or always out of line (its length decides), so equal
// strings hash equally whatever vector they come from.
inline uint64_t HashString(const string_t& s) noexcept {
    if (s.IsInlined()) {
        uint64_t w[2];
        std::memcpy(w, &s, sizeof(w));
        return HashInt64(w[0] * 0x9e3779b97f4a7c15ULL + w[1]);
    }
    return HashBytes(s.data(), s.size());
}
} // namespace

uint64_t HashBytes(const char* data, size_t length) noexcept {
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(length) * 0xff51afd7ed558ccdULL);
    size_t n = length;
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, data, 8);
        h = (h ^ HashInt64(w)) * 0x9fb21c651e98df25ULL;
        h ^= h >> 31;
        data += 8;
        n -= 8;
    }
    if (n > 0) {
        uint64_t w = 0;
        std::memcpy(&w, data, n);
        h = (h ^ HashInt64(w ^ (static_cast<uint64_t>(n) << 56))) * 0x9fb21c651e98df25ULL;
    }
    return HashInt64(h);
}

void HashVector(const Vector& v, idx_t count, uint64_t* out, bool combine) {
    UnifiedFormat u;
    v.ToUnified(u);
    DispatchPhysical(v.type().physical(), [&](auto tag) {
        using T = decltype(tag);
        const T* data = u.Data<T>();
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
            if (v.format() == VectorFormat::Flat &&
                u.validity->AllValid()) { // integers/dates: vectorised
                if constexpr (std::is_same_v<T, int32_t>) {
                    combine ? kernels::CombineInt32Column(data, count, out)
                            : kernels::HashInt32Column(data, count, out);
                } else {
                    combine ? kernels::CombineInt64Column(data, count, out)
                            : kernels::HashInt64Column(data, count, out);
                }
                return;
            }
        }
        if constexpr (std::is_same_v<T, string_t>) {
            if (v.format() == VectorFormat::Dictionary) {
                // Dictionary-encoded strings (a handful of distinct values behind thousands of
                // rows): hash each distinct entry once and look the rest up.
                uint64_t memo[kVectorSize];
                bool known[kVectorSize] = {};
                for (idx_t i = 0; i < count; i++) {
                    uint64_t h = kNullHash;
                    if (u.IsValid(i)) {
                        const sel_t code = u.sel[i];
                        if (!known[code]) {
                            memo[code] = HashString(data[code]);
                            known[code] = true;
                        }
                        h = memo[code];
                    }
                    out[i] = combine ? CombineHash(out[i], h) : h;
                }
                return;
            }
        }
        for (idx_t i = 0; i < count; i++) {
            uint64_t h;
            if (!u.IsValid(i)) {
                h = kNullHash;
            } else {
                const T& x = data[u.sel[i]];
                if constexpr (std::is_same_v<T, string_t>) {
                    h = HashString(x);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (std::isnan(x)) {
                        h = kNaNHash;
                    } else if (x == 0.0) {
                        h = HashInt64(0);
                    } else {
                        uint64_t bits;
                        std::memcpy(&bits, &x, sizeof(bits));
                        h = HashInt64(bits);
                    }
                } else if constexpr (std::is_same_v<T, bool>) {
                    h = HashInt64(x ? 2 : 1);
                } else {
                    h = HashInt64(static_cast<uint64_t>(static_cast<int64_t>(x)));
                }
            }
            out[i] = combine ? CombineHash(out[i], h) : h;
        }
    });
}

void HashColumns(const Vector* const* columns, size_t column_count, idx_t count, uint64_t* out) {
    if (column_count == 0) {
        std::memset(out, 0, count * sizeof(uint64_t));
        return;
    }
    for (size_t c = 0; c < column_count; c++) {
        HashVector(*columns[c], count, out, c > 0);
    }
}

} // namespace cdb
