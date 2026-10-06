#include "execution/hashing.h"

#include "execution/type_dispatch.h"

#include <cmath>
#include <cstring>

namespace cdb {

namespace {
constexpr uint64_t kNullHash = 0x6a09e667f3bcc909ULL;
constexpr uint64_t kNaNHash = 0xbb67ae8584caa73bULL;
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
        for (idx_t i = 0; i < count; i++) {
            uint64_t h;
            if (!u.IsValid(i)) {
                h = kNullHash;
            } else {
                const T& x = data[u.sel[i]];
                if constexpr (std::is_same_v<T, string_t>) {
                    h = HashBytes(x.data(), x.size());
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
