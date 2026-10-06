#include "kernels/cpu.h"

#include <atomic>
#include <cstdlib>

namespace cdb::kernels {

namespace {
std::atomic<int>& SimdState() {
    static std::atomic<int> state{std::getenv("CDB_NO_SIMD") != nullptr ? 0 : 1};
    return state;
}
} // namespace

bool CpuHasAvx2() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    static const bool has = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
    }();
    return has;
#else
    return false;
#endif
}

bool UseAvx2() noexcept {
    return SimdState().load(std::memory_order_relaxed) != 0 && CpuHasAvx2();
}

bool UseSse42Crc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    static const bool has = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("sse4.2") != 0;
    }();
    return has && SimdState().load(std::memory_order_relaxed) != 0;
#else
    return false;
#endif
}

void SetSimdEnabled(bool enabled) noexcept {
    SimdState().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

} // namespace cdb::kernels
