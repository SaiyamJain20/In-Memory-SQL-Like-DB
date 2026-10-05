#include "vector/selection_vector.h"

#include <array>
#include <cstring>

namespace cdb {

namespace {

struct StaticSelections {
    std::array<sel_t, kVectorSize> identity{};
    std::array<sel_t, kVectorSize> zeros{};
    StaticSelections() {
        for (idx_t i = 0; i < kVectorSize; i++) {
            identity[i] = static_cast<sel_t>(i);
        }
    }
};

const StaticSelections& Statics() {
    static const StaticSelections statics;
    return statics;
}

} // namespace

const SelectionVector& SelectionVector::Identity() {
    static const SelectionVector identity(Statics().identity.data(), kVectorSize);
    return identity;
}

const SelectionVector& SelectionVector::Zeros() {
    static const SelectionVector zeros(Statics().zeros.data(), kVectorSize);
    return zeros;
}

SelectionVector SelectionVector::Copy(idx_t count) const {
    CDB_ASSERT(count <= capacity_);
    SelectionVector copy(count);
    if (count > 0) {
        std::memcpy(copy.data_, data_, count * sizeof(sel_t));
    }
    return copy;
}

} // namespace cdb
