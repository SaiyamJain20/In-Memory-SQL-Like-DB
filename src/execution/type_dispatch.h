#pragma once

#include "common/assert.h"
#include "types/logical_type.h"
#include "types/string_t.h"

namespace cdb {

// Calls f(T{}) with T the C++ element type of `type`: bool, int32_t, int64_t, double or string_t.
// DATE is stored as int32_t. All branches must return the same type.
template <class F> decltype(auto) DispatchPhysical(PhysicalType type, F&& f) {
    switch (type) {
    case PhysicalType::Bool:
        return f(bool{});
    case PhysicalType::Int32:
        return f(int32_t{});
    case PhysicalType::Int64:
        return f(int64_t{});
    case PhysicalType::Double:
        return f(double{});
    case PhysicalType::String:
        return f(string_t{});
    }
    CDB_UNREACHABLE("DispatchPhysical");
}

} // namespace cdb
