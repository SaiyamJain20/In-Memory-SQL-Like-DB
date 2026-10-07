#include "storage/hyperloglog.h"

#include <algorithm>
#include <cmath>

namespace cdb {

namespace {
// The estimate (in multiples of the number of registers) up to which linear counting is used.
constexpr double kLinearCountingLimit = 2.7;
} // namespace

bool HyperLogLog::FromRegisters(std::vector<uint8_t> registers, HyperLogLog& out) {
    if (registers.size() != kRegisters) {
        return false;
    }
    if (std::any_of(registers.begin(), registers.end(),
                    [](uint8_t r) { return r > kMaxRegister; })) {
        return false;
    }
    out.registers_ = std::move(registers);
    return true;
}

void HyperLogLog::Merge(const HyperLogLog& other) noexcept {
    for (size_t i = 0; i < kRegisters; i++) {
        registers_[i] = std::max(registers_[i], other.registers_[i]);
    }
}

bool HyperLogLog::Empty() const noexcept {
    return NonZeroRegisters() == 0;
}

size_t HyperLogLog::NonZeroRegisters() const noexcept {
    return static_cast<size_t>(
        std::count_if(registers_.begin(), registers_.end(), [](uint8_t r) { return r != 0; }));
}

double HyperLogLog::Estimate() const noexcept {
    constexpr double m = static_cast<double>(kRegisters);
    double sum = 0;
    size_t zeros = 0;
    for (const uint8_t r : registers_) {
        sum += std::ldexp(1.0, -static_cast<int>(r));
        zeros += r == 0;
    }
    if (zeros == kRegisters) {
        return 0;
    }
    if (zeros > 0) {
        // Linear counting from the number of empty registers is unbiased and more accurate than the
        // harmonic mean while many registers are still empty: measured RMS error 1.7% against 2.9%
        // at 10,000 values (m = 4096), equal at about 2.75 m values, worse beyond. The decision is
        // taken on its own estimate (the raw estimator is biased by +2.6% around there).
        const double linear = m * std::log(m / static_cast<double>(zeros));
        if (linear <= kLinearCountingLimit * m) {
            return linear;
        }
    }
    const double alpha = 0.7213 / (1.0 + 1.079 / m);
    return alpha * m * m / sum;
}

} // namespace cdb
