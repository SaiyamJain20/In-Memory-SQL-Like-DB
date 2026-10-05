// Phase 2 storage benchmarks: append throughput, zero-copy scan throughput by projection width,
// and zone-map pruning. Numbers are recorded in docs/BENCHMARKS.md.

#include "storage/table.h"
#include "test_util.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

namespace cdb {

namespace {

// A lineitem-like schema: sorted-ish id, quantity, price, shipdate, short and long strings.
std::vector<ColumnDefinition> BenchSchema() {
    return {{"id", LogicalType::BigInt()},    {"qty", LogicalType::Integer()},
            {"price", LogicalType::Double()}, {"shipdate", LogicalType::Date()},
            {"flag", LogicalType::Varchar()}, {"comment", LogicalType::Varchar()}};
}

// Fills `chunk` with rows [first, first + n) of a deterministic synthetic dataset.
void FillChunk(DataChunk& chunk, idx_t first, idx_t n) {
    chunk.Reset();
    auto* id = chunk.column(0).FlatData<int64_t>();
    auto* qty = chunk.column(1).FlatData<int32_t>();
    auto* price = chunk.column(2).FlatData<double>();
    auto* date = chunk.column(3).FlatData<int32_t>();
    for (idx_t i = 0; i < n; i++) {
        const idx_t row = first + i;
        id[i] = static_cast<int64_t>(row);
        qty[i] = static_cast<int32_t>(1 + (row * 7) % 50);
        price[i] = 900.0 + static_cast<double>((row * 13) % 100000) / 10.0;
        date[i] = 8035 + static_cast<int32_t>(row / 2500); // ascending => tight zone maps
        chunk.column(4).FlatData<string_t>()[i] = string_t::FromView(row % 3 == 0 ? "A" : "R");
        chunk.column(5).FlatData<string_t>()[i] = chunk.column(5).AddString(
            "comment text for row " + std::to_string(row) + " with padding");
    }
    chunk.SetCardinality(n);
}

constexpr idx_t kRows = 2'000'000;

std::vector<LogicalType> SchemaTypes() {
    std::vector<LogicalType> types;
    for (const auto& c : BenchSchema())
        types.push_back(c.type);
    return types;
}

std::unique_ptr<Table> BuildTable() {
    auto table = std::make_unique<Table>("bench", BenchSchema());
    DataChunk chunk;
    chunk.Initialize(SchemaTypes());
    for (idx_t first = 0; first < kRows; first += kVectorSize) {
        FillChunk(chunk, first, std::min(kVectorSize, kRows - first));
        table->Append(chunk);
    }
    return table;
}

// Shared across benchmarks: building 2M rows once keeps the run short.
const Table& SharedTable() {
    static const std::unique_ptr<Table> table = BuildTable();
    return *table;
}

} // namespace

static void BM_Table_Append(benchmark::State& st) {
    DataChunk chunk;
    chunk.Initialize(SchemaTypes());
    constexpr idx_t kAppendRows = 200 * kVectorSize;
    for (auto _ : st) {
        Table table("t", BenchSchema());
        for (idx_t first = 0; first < kAppendRows; first += kVectorSize) {
            FillChunk(chunk, first, kVectorSize); // (data generation is part of the measurement)
            table.Append(chunk);
        }
        benchmark::DoNotOptimize(table.RowCount());
    }
    st.SetItemsProcessed(static_cast<int64_t>(st.iterations()) * static_cast<int64_t>(kAppendRows));
}
BENCHMARK(BM_Table_Append)->Unit(benchmark::kMillisecond);

// Full scan of `range(0)` columns (the leading ones of {id, qty, price, shipdate}); every value
// is touched with a trivial reduction so the zero-copy views cannot be optimised away.
static void BM_Table_Scan_Columns(benchmark::State& st) {
    const Table& table = SharedTable();
    const idx_t ncols = static_cast<idx_t>(st.range(0));
    std::vector<idx_t> cols;
    for (idx_t c = 0; c < ncols; c++)
        cols.push_back(c);
    auto snap = table.Snapshot();
    for (auto _ : st) {
        TableScan scan(snap, cols);
        DataChunk chunk;
        chunk.Initialize(scan.types());
        int64_t sum = 0;
        while (scan.Next(chunk)) {
            const Vector& v = chunk.column(0);
            const auto* ids = reinterpret_cast<const int64_t*>(v.FlatBytes());
            for (idx_t i = 0; i < chunk.size(); i++)
                sum += ids[i];
        }
        benchmark::DoNotOptimize(sum);
    }
    st.SetItemsProcessed(static_cast<int64_t>(st.iterations()) * static_cast<int64_t>(kRows));
}
BENCHMARK(BM_Table_Scan_Columns)->Arg(1)->Arg(2)->Arg(4)->Unit(benchmark::kMillisecond);

// Scan with no work per row: isolates the cost of the scan machinery itself (view hand-out).
static void BM_Table_Scan_MachineryOnly(benchmark::State& st) {
    const Table& table = SharedTable();
    auto snap = table.Snapshot();
    for (auto _ : st) {
        TableScan scan(snap, {0, 1, 2, 3});
        DataChunk chunk;
        chunk.Initialize(scan.types());
        idx_t rows = 0;
        while (scan.Next(chunk))
            rows += chunk.size();
        benchmark::DoNotOptimize(rows);
    }
    st.SetItemsProcessed(static_cast<int64_t>(st.iterations()) * static_cast<int64_t>(kRows));
}
BENCHMARK(BM_Table_Scan_MachineryOnly)->Unit(benchmark::kMillisecond);

// shipdate is ascending, so `shipdate >= X` prunes a growing prefix of the row groups.
// range(0) = fraction of the table (in percent) that can be skipped.
static void BM_Table_Scan_ZoneMapPruned(benchmark::State& st) {
    const Table& table = SharedTable();
    auto snap = table.Snapshot();
    const int skip_percent = static_cast<int>(st.range(0));
    const int32_t first_date = 8035;
    const int32_t last_date = 8035 + static_cast<int32_t>((kRows - 1) / 2500);
    const int32_t threshold = first_date + (last_date - first_date) * skip_percent / 100;
    idx_t scanned = 0, total = 0;
    for (auto _ : st) {
        TableScan scan(snap, {0, 3}, {{3, CompareOp::Ge, Value::Date(date_t{threshold})}});
        DataChunk chunk;
        chunk.Initialize(scan.types());
        idx_t rows = 0;
        while (scan.Next(chunk))
            rows += chunk.size();
        benchmark::DoNotOptimize(rows);
        scanned = scan.row_groups_scanned();
        total = scan.row_groups_scanned() + scan.row_groups_skipped();
    }
    st.counters["groups_scanned"] = static_cast<double>(scanned);
    st.counters["groups_total"] = static_cast<double>(total);
}
BENCHMARK(BM_Table_Scan_ZoneMapPruned)
    ->Arg(0)
    ->Arg(50)
    ->Arg(90)
    ->Arg(99)
    ->Unit(benchmark::kMicrosecond);

static void BM_Table_Snapshot(benchmark::State& st) {
    const Table& table = SharedTable();
    for (auto _ : st)
        benchmark::DoNotOptimize(table.Snapshot());
}
BENCHMARK(BM_Table_Snapshot)->Unit(benchmark::kNanosecond);

} // namespace cdb
