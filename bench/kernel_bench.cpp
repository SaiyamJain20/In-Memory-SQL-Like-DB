// Phase 5 benchmarks: bit unpacking, the AVX2 kernels against their scalar versions (the same code
// path with SIMD switched off), and the per-vector cost of scanning encoded segments against raw
// ones. "ns/row" counters divide the iteration time by the rows it processed.

#include "kernels/aggregate.h"
#include "kernels/cpu.h"
#include "kernels/decode.h"
#include "kernels/hash.h"
#include "kernels/select.h"
#include "storage/bitpacking.h"
#include "storage/column_builder.h"
#include "storage/encoding.h"

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace cdb {

namespace {

constexpr idx_t kN = kVectorSize;

void PerRow(benchmark::State& state, idx_t rows_per_iteration) {
    state.counters["ns/row"] = benchmark::Counter(static_cast<double>(rows_per_iteration),
                                                  benchmark::Counter::kIsIterationInvariantRate |
                                                      benchmark::Counter::kInvert);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(rows_per_iteration));
}

struct SimdMode {
    explicit SimdMode(bool on) { kernels::SetSimdEnabled(on); }
    ~SimdMode() { kernels::SetSimdEnabled(true); }
};

// ---------------------------------------------------------------------------------- unpack

// The generic loop the specialised unpackers replaced (two loads, a branch and a mask per value).
void UnpackGenericBaseline(const uint8_t* in, idx_t count, uint8_t width, uint64_t* out) {
    const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    for (idx_t i = 0; i < count; i++) {
        const idx_t bit = i * width;
        const size_t byte = static_cast<size_t>(bit >> 6) * 8;
        const unsigned shift = static_cast<unsigned>(bit & 63);
        uint64_t lo, hi;
        std::memcpy(&lo, in + byte, 8);
        std::memcpy(&hi, in + byte + 8, 8);
        uint64_t v = lo >> shift;
        if (shift + width > 64) {
            v |= hi << (64 - shift);
        }
        out[i] = v & mask;
    }
}

std::vector<uint8_t> PackedColumn(uint8_t width) {
    std::mt19937_64 rng(1);
    std::vector<uint64_t> values(kN);
    const uint64_t mask = (uint64_t{1} << width) - 1;
    for (auto& v : values) {
        v = rng() & mask;
    }
    std::vector<uint8_t> packed(PackedBytes(kN, width), 0);
    BitPack(values.data(), kN, width, packed.data());
    return packed;
}

void BM_UnpackGenericBaseline(benchmark::State& state) {
    const auto width = static_cast<uint8_t>(state.range(0));
    const auto packed = PackedColumn(width);
    std::vector<uint64_t> out(kN);
    for (auto _ : state) {
        UnpackGenericBaseline(packed.data(), kN, width, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
void BM_UnpackSpecialised(benchmark::State& state) {
    const auto width = static_cast<uint8_t>(state.range(0));
    const auto packed = PackedColumn(width);
    std::vector<uint64_t> out(kN);
    for (auto _ : state) {
        BitUnpack(packed.data(), kN, width, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
BENCHMARK(BM_UnpackGenericBaseline)->Arg(3)->Arg(8)->Arg(12)->Arg(17)->Arg(24)->Arg(33);
BENCHMARK(BM_UnpackSpecialised)->Arg(3)->Arg(8)->Arg(12)->Arg(17)->Arg(24)->Arg(33);

// ---------------------------------------------------------------------------------- decode
// conversions

void BM_OffsetsToInt32(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::vector<uint64_t> off(kN);
    std::mt19937_64 rng(2);
    for (auto& o : off) {
        o = rng() & 0xFFFFF;
    }
    std::vector<int32_t> out(kN + 8);
    for (auto _ : state) {
        kernels::OffsetsToInt32(off.data(), kN, 1000, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
void BM_OffsetsToScaledDouble(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::vector<uint64_t> off(kN);
    std::mt19937_64 rng(3);
    for (auto& o : off) {
        o = rng() & 0xFFFFFF;
    }
    std::vector<double> out(kN + 4);
    for (auto _ : state) {
        kernels::OffsetsToScaledDouble(off.data(), kN, 12345, 100.0, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
BENCHMARK(BM_OffsetsToInt32)->Arg(0)->Arg(1);
BENCHMARK(BM_OffsetsToScaledDouble)->Arg(0)->Arg(1);

// ---------------------------------------------------------------------------------- selection

template <class T> void SelectBench(benchmark::State& state, kernels::CmpOp op) {
    const SimdMode mode(state.range(0) != 0);
    const int selectivity_percent = static_cast<int>(state.range(1));
    std::mt19937_64 rng(4);
    std::vector<T> data(kN);
    for (auto& x : data) {
        x = static_cast<T>(rng() % 100);
    }
    std::vector<sel_t> out(kN);
    for (auto _ : state) {
        idx_t n = kernels::SelectConstant<T>(op, data.data(), kN,
                                             static_cast<T>(selectivity_percent), out.data());
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
void BM_SelectLtInt32(benchmark::State& s) {
    SelectBench<int32_t>(s, kernels::CmpOp::Lt);
}
void BM_SelectLtInt64(benchmark::State& s) {
    SelectBench<int64_t>(s, kernels::CmpOp::Lt);
}
void BM_SelectLtDouble(benchmark::State& s) {
    SelectBench<double>(s, kernels::CmpOp::Lt);
}
BENCHMARK(BM_SelectLtInt32)->ArgsProduct({{0, 1}, {2, 50, 98}});
BENCHMARK(BM_SelectLtInt64)->ArgsProduct({{0, 1}, {2, 50, 98}});
BENCHMARK(BM_SelectLtDouble)->ArgsProduct({{0, 1}, {2, 50, 98}});

// ---------------------------------------------------------------------------------- aggregates

void BM_SumInt32(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::vector<int32_t> data(kN, 3);
    for (auto _ : state) {
        benchmark::DoNotOptimize(kernels::SumInt32(data.data(), kN));
    }
    PerRow(state, kN);
}
void BM_SumInt64Checked(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::vector<int64_t> data(kN, 3);
    for (auto _ : state) {
        int64_t sum = 0;
        benchmark::DoNotOptimize(kernels::AddSumInt64(data.data(), kN, &sum));
        benchmark::DoNotOptimize(sum);
    }
    PerRow(state, kN);
}
void BM_SumDouble(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::vector<double> data(kN, 1.5);
    for (auto _ : state) {
        benchmark::DoNotOptimize(kernels::SumDouble(data.data(), kN));
    }
    PerRow(state, kN);
}
void BM_MinMaxInt32(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::mt19937_64 rng(5);
    std::vector<int32_t> data(kN);
    for (auto& x : data) {
        x = static_cast<int32_t>(rng());
    }
    for (auto _ : state) {
        int32_t lo, hi;
        kernels::MinMaxInt32(data.data(), kN, &lo, &hi);
        benchmark::DoNotOptimize(lo);
        benchmark::DoNotOptimize(hi);
    }
    PerRow(state, kN);
}
void BM_MinMaxInt64(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::mt19937_64 rng(6);
    std::vector<int64_t> data(kN);
    for (auto& x : data) {
        x = static_cast<int64_t>(rng());
    }
    for (auto _ : state) {
        int64_t lo, hi;
        kernels::MinMaxInt64(data.data(), kN, &lo, &hi);
        benchmark::DoNotOptimize(lo);
        benchmark::DoNotOptimize(hi);
    }
    PerRow(state, kN);
}
BENCHMARK(BM_SumInt32)->Arg(0)->Arg(1);
BENCHMARK(BM_SumInt64Checked)->Arg(0)->Arg(1);
BENCHMARK(BM_SumDouble)->Arg(0)->Arg(1);
BENCHMARK(BM_MinMaxInt32)->Arg(0)->Arg(1);
BENCHMARK(BM_MinMaxInt64)->Arg(0)->Arg(1);

// ---------------------------------------------------------------------------------- hashing

void BM_HashInt64Column(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::mt19937_64 rng(7);
    std::vector<int64_t> data(kN);
    for (auto& x : data) {
        x = static_cast<int64_t>(rng());
    }
    std::vector<uint64_t> out(kN + 4);
    for (auto _ : state) {
        kernels::HashInt64Column(data.data(), kN, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
void BM_HashInt32Column(benchmark::State& state) {
    const SimdMode mode(state.range(0) != 0);
    std::mt19937_64 rng(8);
    std::vector<int32_t> data(kN);
    for (auto& x : data) {
        x = static_cast<int32_t>(rng());
    }
    std::vector<uint64_t> out(kN + 4);
    for (auto _ : state) {
        kernels::HashInt32Column(data.data(), kN, out.data());
        benchmark::DoNotOptimize(out.data());
    }
    PerRow(state, kN);
}
BENCHMARK(BM_HashInt64Column)->Arg(0)->Arg(1);
BENCHMARK(BM_HashInt32Column)->Arg(0)->Arg(1);

// ---------------------------------------------------------------------------------- scanning
// segments

// One 8-vector segment built with compression on or off; the benchmark scans one vector of it.
std::shared_ptr<ColumnSegment> BuildSegment(LogicalType type, bool compress,
                                            const std::vector<Value>& values) {
    SetCompressionEnabled(compress);
    ColumnBuilder builder(type, AlignUp(values.size(), kVectorSize));
    Vector src(type, kVectorSize);
    for (idx_t at = 0; at < values.size(); at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, values.size() - at);
        for (idx_t i = 0; i < n; i++) {
            src.SetValue(i, values[at + i]);
        }
        builder.Append(src, 0, n);
    }
    auto seg = builder.Seal();
    SetCompressionEnabled(true);
    return seg;
}

void ScanBench(benchmark::State& state, LogicalType type, const std::vector<Value>& values) {
    const bool compress = state.range(0) != 0;
    const auto seg = BuildSegment(type, compress, values);
    Vector out(type, kVectorSize);
    idx_t at = 0;
    for (auto _ : state) {
        seg->Scan(at, kVectorSize, out);
        benchmark::DoNotOptimize(out.format());
        at = (at + kVectorSize) % (values.size() - kVectorSize + 1) / kVectorSize * kVectorSize;
        if (at + kVectorSize > values.size()) {
            at = 0;
        }
    }
    state.counters["bytes/row"] =
        static_cast<double>(seg->MemoryUsage()) / static_cast<double>(values.size());
    PerRow(state, kVectorSize);
}

void BM_ScanDateFor(benchmark::State& state) { // dates over ~7 years: 12 bits
    std::mt19937_64 rng(9);
    std::vector<Value> v;
    for (idx_t i = 0; i < 8 * kVectorSize; i++) {
        v.push_back(Value::Date(date_t{8000 + static_cast<int32_t>(rng() % 2500)}));
    }
    ScanBench(state, LogicalType::Date(), v);
}
void BM_ScanMoneyDouble(benchmark::State& state) { // two decimals, up to 100000.00: 24 bits
    std::mt19937_64 rng(10);
    std::vector<Value> v;
    for (idx_t i = 0; i < 8 * kVectorSize; i++) {
        v.push_back(Value::Double(static_cast<double>(rng() % 10000000) / 100.0));
    }
    ScanBench(state, LogicalType::Double(), v);
}
void BM_ScanSortedKeyDelta(benchmark::State& state) { // an ascending key column
    std::vector<Value> v;
    int64_t cur = 1000000;
    std::mt19937_64 rng(11);
    for (idx_t i = 0; i < 8 * kVectorSize; i++) {
        cur += static_cast<int64_t>(rng() % 4);
        v.push_back(Value::BigInt(cur));
    }
    ScanBench(state, LogicalType::BigInt(), v);
}
void BM_ScanDictionaryStrings(benchmark::State& state) { // 7 distinct values, like l_shipmode
    static const char* const kModes[] = {"AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB", "REG AIR"};
    std::mt19937_64 rng(12);
    std::vector<Value> v;
    for (idx_t i = 0; i < 8 * kVectorSize; i++) {
        v.push_back(Value::Varchar(kModes[rng() % 7]));
    }
    ScanBench(state, LogicalType::Varchar(), v);
}
BENCHMARK(BM_ScanDateFor)->Arg(0)->Arg(1);
BENCHMARK(BM_ScanMoneyDouble)->Arg(0)->Arg(1);
BENCHMARK(BM_ScanSortedKeyDelta)->Arg(0)->Arg(1);
BENCHMARK(BM_ScanDictionaryStrings)->Arg(0)->Arg(1);

} // namespace

} // namespace cdb
