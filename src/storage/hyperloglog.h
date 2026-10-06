#pragma once

#include "common/types.h"

#include <cstdint>
#include <vector>

namespace cdb {

// A HyperLogLog sketch (Flajolet et al.): estimates the number of distinct values it was shown
// from 2^kPrecision one-byte registers. Every sealed column segment carries one, so the number of
// distinct values of a whole column is the estimate of the merge of its segments' sketches:
// merging is a per-register maximum, exact for the union of the inputs, and needs neither the
// values nor any ordering. The relative standard error is 1.04 / sqrt(2^kPrecision) = 1.6%.
//
// Callers pass well-mixed 64-bit hashes (HashInt64 / HashBytes); equal values must hash equally.
class HyperLogLog {
  public:
    static constexpr unsigned kPrecision = 12;
    static constexpr size_t kRegisters = size_t{1} << kPrecision;
    // A register holds the position of the first set bit among the 64 - kPrecision hash bits it
    // saw, so at most 64 - kPrecision + 1.
    static constexpr uint8_t kMaxRegister = 64 - kPrecision + 1;

    HyperLogLog() : registers_(kRegisters, 0) {}

    // Builds a sketch from stored registers; false if there are not kRegisters of them or one is
    // out of range (a damaged file).
    static bool FromRegisters(std::vector<uint8_t> registers, HyperLogLog& out);

    void Add(uint64_t hash) noexcept {
        const size_t index = static_cast<size_t>(hash >> (64 - kPrecision));
        // the remaining bits, with a sentinel so that the count of leading zeros is bounded
        const uint64_t rest = (hash << kPrecision) | (uint64_t{1} << (kPrecision - 1));
        const auto rank = static_cast<uint8_t>(__builtin_clzll(rest) + 1);
        if (rank > registers_[index]) {
            registers_[index] = rank;
        }
    }

    // this = this UNION other.
    void Merge(const HyperLogLog& other) noexcept;

    bool Empty() const noexcept;
    size_t NonZeroRegisters() const noexcept;

    // The estimated number of distinct hashes added (0 for an empty sketch). Linear counting while
    // many registers are still empty, the harmonic-mean estimator above that.
    double Estimate() const noexcept;

    const std::vector<uint8_t>& registers() const noexcept { return registers_; }
    bool operator==(const HyperLogLog& other) const noexcept {
        return registers_ == other.registers_;
    }

  private:
    std::vector<uint8_t> registers_;
};

} // namespace cdb
