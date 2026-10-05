// Micro-benchmarks for the Phase 1 data model. Numbers are recorded in docs/BENCHMARKS.md.
// Build with the `release` preset; run: build/release/bench/cdb_bench

#include "memory/arena.h"
#include "test_util.h"
#include "types/date.h"
#include "types/string_t.h"
#include "vector/data_chunk.h"
#include "vector/validity_mask.h"
#include "vector/vector.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

namespace cdb {

namespace {

constexpr idx_t kRows = kVectorSize;

void ItemsAreRows(benchmark::State& st, idx_t rows_per_iteration) {
    st.SetItemsProcessed(static_cast<int64_t>(st.iterations()) *
                         static_cast<int64_t>(rows_per_iteration));
}

SelectionVector RandomSelection(idx_t count, idx_t range, uint64_t seed) {
    test::Rng rng(seed);
    SelectionVector sel(count);
    for (idx_t i = 0; i < count; i++)
        sel.Set(i, static_cast<sel_t>(test::RandBelow(rng, range)));
    return sel;
}

Vector MakeNumeric(LogicalType type) {
    test::Rng rng(1);
    Vector v(type, kRows);
    for (idx_t i = 0; i < kRows; i++)
        v.SetValue(i, test::RandomValue(rng, type, 0.0));
    return v;
}

} // namespace

// ------------------------------------------------------------------ validity

static void BM_ValidityMask_CountValid(benchmark::State& st) {
    ValidityMask m(kRows);
    test::Rng rng(1);
    for (idx_t i = 0; i < kRows; i++) {
        if (test::Chance(rng, 0.1))
            m.SetInvalid(i);
    }
    for (auto _ : st)
        benchmark::DoNotOptimize(m.CountValid(kRows));
    ItemsAreRows(st, kRows);
}
BENCHMARK(BM_ValidityMask_CountValid);

static void BM_ValidityMask_IsValid_Scan(benchmark::State& st) {
    ValidityMask m(kRows);
    test::Rng rng(1);
    for (idx_t i = 0; i < kRows; i++) {
        if (test::Chance(rng, 0.1))
            m.SetInvalid(i);
    }
    for (auto _ : st) {
        idx_t valid = 0;
        for (idx_t i = 0; i < kRows; i++)
            valid += m.IsValid(i);
        benchmark::DoNotOptimize(valid);
    }
    ItemsAreRows(st, kRows);
}
BENCHMARK(BM_ValidityMask_IsValid_Scan);

// ------------------------------------------------------------------ strings

static void BM_StringT_Compare(benchmark::State& st) {
    // arg 0: both inlined, differ in first 4 bytes. arg 1: out-of-line, equal 100-byte prefix.
    const bool long_equal_prefix = st.range(0) == 1;
    test::Rng rng(1);
    std::vector<std::string> storage;
    std::vector<string_t> a, b;
    storage.reserve(2 * 1024);
    for (int i = 0; i < 1024; i++) {
        std::string x = long_equal_prefix ? std::string(100, 'p') + std::to_string(i % 7)
                                          : "k" + std::to_string(test::RandBelow(rng, 100000));
        std::string y = long_equal_prefix ? std::string(100, 'p') + std::to_string((i + 1) % 7)
                                          : "k" + std::to_string(test::RandBelow(rng, 100000));
        storage.push_back(std::move(x));
        storage.push_back(std::move(y));
    }
    for (int i = 0; i < 1024; i++) {
        a.push_back(string_t::FromView(storage[2 * i]));
        b.push_back(string_t::FromView(storage[2 * i + 1]));
    }
    for (auto _ : st) {
        int acc = 0;
        for (int i = 0; i < 1024; i++)
            acc += string_t::Compare(a[i], b[i]);
        benchmark::DoNotOptimize(acc);
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_StringT_Compare)->Arg(0)->Arg(1);

static void BM_StringT_Equal(benchmark::State& st) {
    const bool is_long = st.range(0) == 1;
    std::vector<std::string> storage;
    for (int i = 0; i < 1024; i++) {
        storage.push_back((is_long ? std::string(40, 'x') : std::string("id")) + std::to_string(i));
    }
    std::vector<string_t> a, b;
    for (const auto& s : storage) {
        a.push_back(string_t::FromView(s));
        b.push_back(string_t::FromView(s));
    }
    for (auto _ : st) {
        int eq = 0;
        for (int i = 0; i < 1024; i++)
            eq += a[i] == b[(i + 1) & 1023];
        benchmark::DoNotOptimize(eq);
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_StringT_Equal)->Arg(0)->Arg(1);

static void BM_StringHeap_Add_Long(benchmark::State& st) {
    const std::string text(40, 'h');
    StringHeap heap;
    for (auto _ : st) {
        for (int i = 0; i < 1000; i++)
            benchmark::DoNotOptimize(heap.Add(text));
        heap.Reset();
    }
    ItemsAreRows(st, 1000);
}
BENCHMARK(BM_StringHeap_Add_Long);

static void BM_Arena_Allocate24(benchmark::State& st) {
    Arena arena;
    for (auto _ : st) {
        for (int i = 0; i < 1000; i++)
            benchmark::DoNotOptimize(arena.Allocate(24, 8));
        arena.Reset();
    }
    ItemsAreRows(st, 1000);
}
BENCHMARK(BM_Arena_Allocate24);

// ------------------------------------------------------------------ dates

static void BM_Date_FromString(benchmark::State& st) {
    std::vector<std::string> texts;
    for (int i = 0; i < 1024; i++) {
        texts.push_back(Date::ToString(date_t{static_cast<int32_t>(i * 17 - 5000)}));
    }
    for (auto _ : st) {
        int64_t sum = 0;
        for (const auto& t : texts)
            sum += Date::FromString(t)->days;
        benchmark::DoNotOptimize(sum);
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_Date_FromString);

static void BM_Date_ToYMD(benchmark::State& st) {
    for (auto _ : st) {
        int64_t sum = 0;
        for (int32_t d = 0; d < 1024; d++) {
            int y, m, day;
            Date::ToYMD(date_t{d * 31}, y, m, day);
            sum += y + m + day;
        }
        benchmark::DoNotOptimize(sum);
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_Date_ToYMD);

// ------------------------------------------------------------------ vectors

static void BM_Vector_Slice(benchmark::State& st) {
    Vector src = MakeNumeric(LogicalType::BigInt());
    SelectionVector sel = RandomSelection(1024, kRows, 7);
    for (auto _ : st) {
        Vector v(LogicalType::BigInt(), kRows);
        v.Reference(src);
        v.Slice(sel, 1024);
        benchmark::DoNotOptimize(v.format());
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_Vector_Slice);

static void BM_Vector_SliceThenFlatten_BigInt(benchmark::State& st) {
    Vector src = MakeNumeric(LogicalType::BigInt());
    SelectionVector sel = RandomSelection(1024, kRows, 7);
    for (auto _ : st) {
        Vector v(LogicalType::BigInt(), kRows);
        v.Reference(src);
        v.Slice(sel, 1024);
        v.Flatten(1024);
        benchmark::DoNotOptimize(v.FlatData<int64_t>());
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_Vector_SliceThenFlatten_BigInt);

static void BM_VectorCopy_Flat(benchmark::State& st) {
    const LogicalType type = st.range(0) == 0 ? LogicalType::Integer() : LogicalType::BigInt();
    Vector src = MakeNumeric(type);
    Vector dst(type, kRows);
    for (auto _ : st) {
        VectorOps::Copy(src, dst, nullptr, kRows);
        benchmark::ClobberMemory();
    }
    ItemsAreRows(st, kRows);
}
BENCHMARK(BM_VectorCopy_Flat)->Arg(0)->Arg(1);

static void BM_VectorCopy_WithSelection_BigInt(benchmark::State& st) {
    Vector src = MakeNumeric(LogicalType::BigInt());
    Vector dst(LogicalType::BigInt(), kRows);
    SelectionVector sel = RandomSelection(1024, kRows, 9);
    for (auto _ : st) {
        VectorOps::Copy(src, dst, &sel, 1024);
        benchmark::ClobberMemory();
    }
    ItemsAreRows(st, 1024);
}
BENCHMARK(BM_VectorCopy_WithSelection_BigInt);

static void BM_VectorCopy_Varchar(benchmark::State& st) {
    const bool is_long = st.range(0) == 1;
    Vector src(LogicalType::Varchar(), kRows);
    for (idx_t i = 0; i < kRows; i++) {
        src.SetValue(i, Value::Varchar((is_long ? std::string(40, 'v') : std::string("s")) +
                                       std::to_string(i)));
    }
    Vector dst(LogicalType::Varchar(), kRows);
    for (auto _ : st) {
        dst.Reset();
        VectorOps::Copy(src, dst, nullptr, kRows);
        benchmark::ClobberMemory();
    }
    ItemsAreRows(st, kRows);
}
BENCHMARK(BM_VectorCopy_Varchar)->Arg(0)->Arg(1);

} // namespace cdb
