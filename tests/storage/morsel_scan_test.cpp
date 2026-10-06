#include "storage/table.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace cdb {

namespace {

using test::AllTypesSchema;
using test::ExpectColumnsEqual;
using test::RandomChunk;
using test::ScanAll;
using test::TableModel;

constexpr idx_t kSmallGroup = 3 * kVectorSize; // 6144-row groups: many boundaries, cheap data

struct Built {
    std::unique_ptr<Table> table;
    TableModel model;
};

Built BuildTable(idx_t total, idx_t group_size, uint64_t seed, double nulls = 0.2) {
    test::Rng rng(seed);
    Built b{std::make_unique<Table>("t", AllTypesSchema(), group_size),
            TableModel(AllTypesSchema().size())};
    idx_t done = 0;
    while (done < total) {
        const idx_t n = std::min<idx_t>(1 + test::RandBelow(rng, kVectorSize), total - done);
        b.table->Append(RandomChunk(b.table->schema(), rng, n, b.model, nulls));
        done += n;
    }
    return b;
}

// One vector read from a morsel, tagged with where it sits in scan order.
struct Piece {
    idx_t morsel;
    idx_t vector;
    std::vector<std::vector<Value>> cols;
};

// Reads everything with `threads` threads claiming morsels, and returns the pieces in scan order.
std::vector<Piece> ReadWithThreads(MorselScan& scan, size_t threads) {
    std::vector<std::vector<Piece>> per_thread(threads);
    const auto work = [&](size_t t) {
        DataChunk chunk;
        chunk.Initialize(scan.types());
        ScanMorsel m;
        while (scan.Next(m)) {
            for (idx_t v = 0; v < MorselScan::VectorCount(m); v++) {
                chunk.Reset();
                const idx_t n = scan.ReadVector(m, v, chunk);
                EXPECT_EQ(n, chunk.size());
                chunk.Verify();
                Piece piece{m.index, v, std::vector<std::vector<Value>>(scan.types().size())};
                for (idx_t c = 0; c < scan.types().size(); c++) {
                    for (idx_t r = 0; r < n; r++) {
                        piece.cols[c].push_back(chunk.GetValue(c, r));
                    }
                }
                per_thread[t].push_back(std::move(piece));
            }
        }
    };
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; t++) {
        pool.emplace_back(work, t);
    }
    work(0);
    for (std::thread& th : pool) {
        th.join();
    }
    std::vector<Piece> all;
    for (auto& v : per_thread) {
        for (Piece& p : v) {
            all.push_back(std::move(p));
        }
    }
    std::sort(all.begin(), all.end(), [](const Piece& a, const Piece& b) {
        return a.morsel != b.morsel ? a.morsel < b.morsel : a.vector < b.vector;
    });
    return all;
}

std::vector<std::vector<Value>> Concatenate(const std::vector<Piece>& pieces, size_t columns) {
    std::vector<std::vector<Value>> out(columns);
    for (const Piece& p : pieces) {
        for (size_t c = 0; c < columns; c++) {
            out[c].insert(out[c].end(), p.cols[c].begin(), p.cols[c].end());
        }
    }
    return out;
}

} // namespace

TEST(MorselScan, EmptyTableHasNoMorsels) {
    Table table("t", AllTypesSchema(), kSmallGroup);
    const auto snap = table.Snapshot();
    MorselScan scan(snap, test::AllColumns(*snap));
    EXPECT_EQ(scan.MorselCount(), 0U);
    EXPECT_EQ(scan.RowCount(), 0U);
    ScanMorsel m;
    for (int i = 0; i < 3; i++) {
        EXPECT_FALSE(scan.Next(m)) << "Next stays false once exhausted";
    }
}

TEST(MorselScan, MorselsPartitionTheRowsExactlyOnce) {
    for (const idx_t total : {idx_t{1}, idx_t{2047}, idx_t{2048}, idx_t{2049}, idx_t{6144},
                              idx_t{6145}, idx_t{20000}}) {
        const Built b = BuildTable(total, kSmallGroup, total);
        const auto snap = b.table->Snapshot();
        for (const idx_t vectors : {idx_t{1}, idx_t{2}, idx_t{3}, idx_t{8}}) {
            MorselScan scan(snap, {0}, {}, vectors * kVectorSize);
            std::vector<ScanMorsel> morsels;
            ScanMorsel m;
            while (scan.Next(m)) {
                morsels.push_back(m);
            }
            ASSERT_EQ(morsels.size(), scan.MorselCount());
            idx_t rows = 0;
            for (idx_t i = 0; i < morsels.size(); i++) {
                const ScanMorsel& x = morsels[i];
                EXPECT_EQ(x.index, i) << "indexes are consecutive in scan order";
                EXPECT_EQ(x.offset % kVectorSize, 0U);
                EXPECT_GT(x.count, 0U);
                EXPECT_LE(x.count, vectors * kVectorSize);
                EXPECT_LE(x.offset + x.count, x.group->count());
                if (i + 1 < morsels.size() && morsels[i + 1].group == x.group) {
                    EXPECT_EQ(morsels[i + 1].offset, x.offset + x.count) << "adjacent, no gap";
                    EXPECT_EQ(x.count, vectors * kVectorSize)
                        << "only the last morsel of a group is short";
                }
                rows += x.count;
            }
            EXPECT_EQ(rows, total) << "total " << total << " vectors " << vectors;
            EXPECT_EQ(scan.RowCount(), total);
        }
    }
}

TEST(MorselScan, OneThreadSeesExactlyWhatTableScanSees) {
    for (const bool compressed : {false, true}) {
        const test::ScopedCompression layout(compressed);
        for (const idx_t total : {idx_t{0}, idx_t{1}, idx_t{2048}, idx_t{6144}, idx_t{6145},
                                  idx_t{3 * kSmallGroup + 17}}) {
            const Built b = BuildTable(total, kSmallGroup, total + 100);
            const auto snap = b.table->Snapshot();
            for (const idx_t vectors : {idx_t{1}, idx_t{2}, idx_t{5}}) {
                MorselScan scan(snap, test::AllColumns(*snap), {}, vectors * kVectorSize);
                const auto pieces = ReadWithThreads(scan, 1);
                ExpectColumnsEqual(Concatenate(pieces, scan.types().size()), b.model.cols,
                                   "total " + std::to_string(total));

                // the chunk boundaries are TableScan's as well (vectors never straddle groups)
                TableScan reference(snap, test::AllColumns(*snap));
                DataChunk chunk;
                chunk.Initialize(reference.types());
                std::vector<idx_t> expected_sizes, got_sizes;
                while (reference.Next(chunk)) {
                    expected_sizes.push_back(chunk.size());
                }
                for (const Piece& p : pieces) {
                    got_sizes.push_back(p.cols[0].size());
                }
                EXPECT_EQ(got_sizes, expected_sizes);
            }
        }
    }
}

TEST(MorselScan, ManyThreadsReadEveryRowExactlyOnce) {
    for (const bool compressed : {false, true}) {
        const test::ScopedCompression layout(compressed);
        const Built b = BuildTable(26000, kSmallGroup, 7);
        const auto snap = b.table->Snapshot();
        for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}}) {
            for (int round = 0; round < 3; round++) {
                MorselScan scan(snap, test::AllColumns(*snap), {}, kVectorSize);
                const auto pieces = ReadWithThreads(scan, threads);
                ExpectColumnsEqual(Concatenate(pieces, scan.types().size()), b.model.cols,
                                   "threads " + std::to_string(threads));
            }
        }
    }
}

TEST(MorselScan, EachMorselIsHandedOutOnce) {
    const Built b = BuildTable(30000, kSmallGroup, 11);
    const auto snap = b.table->Snapshot();
    for (const size_t threads : {size_t{2}, size_t{8}}) {
        MorselScan scan(snap, {1}, {}, kVectorSize);
        std::vector<std::atomic<int>> claimed(scan.MorselCount());
        std::vector<std::thread> pool;
        for (size_t t = 0; t < threads; t++) {
            pool.emplace_back([&] {
                ScanMorsel m;
                while (scan.Next(m)) {
                    claimed[m.index]++;
                }
            });
        }
        for (std::thread& th : pool) {
            th.join();
        }
        for (idx_t i = 0; i < claimed.size(); i++) {
            ASSERT_EQ(claimed[i].load(), 1) << "morsel " << i;
        }
    }
}

TEST(MorselScan, ZoneMapPruningMatchesTableScan) {
    // Ascending integers so every group has a distinct range and filters prune a prefix/suffix.
    Table table("t", {{"x", LogicalType::Integer()}, {"y", LogicalType::BigInt()}}, kSmallGroup);
    for (idx_t at = 0; at < 10 * kSmallGroup; at += kVectorSize) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::Integer(), LogicalType::BigInt()});
        for (idx_t r = 0; r < kVectorSize; r++) {
            chunk.SetValue(0, r, Value::Integer(static_cast<int32_t>(at + r)));
            chunk.SetValue(1, r, Value::BigInt(static_cast<int64_t>(at + r) * 3));
        }
        chunk.SetCardinality(kVectorSize);
        table.Append(chunk);
    }
    const auto snap = table.Snapshot();
    for (const CompareOp op : test::AllOps()) {
        for (const int32_t constant : {-1, 0, 5000, static_cast<int32_t>(5 * kSmallGroup),
                                       static_cast<int32_t>(10 * kSmallGroup) - 1,
                                       static_cast<int32_t>(10 * kSmallGroup), 1 << 30}) {
            const std::vector<TableFilter> filters = {{0, op, Value::Integer(constant)}};
            TableScan reference(snap, {0, 1}, filters);
            DataChunk chunk;
            chunk.Initialize(reference.types());
            idx_t reference_rows = 0;
            while (reference.Next(chunk)) {
                reference_rows += chunk.size();
            }
            MorselScan scan(snap, {0, 1}, filters, 2 * kVectorSize);
            EXPECT_EQ(scan.row_groups_scanned(), reference.row_groups_scanned());
            EXPECT_EQ(scan.row_groups_skipped(), reference.row_groups_skipped());
            EXPECT_EQ(scan.RowCount(), reference_rows);
        }
    }
}

TEST(MorselScan, ProjectionReadsOnlyTheRequestedColumnsInOrder) {
    const Built b = BuildTable(9000, kSmallGroup, 13);
    const auto snap = b.table->Snapshot();
    MorselScan scan(snap, {5, 1, 5}, {}, 2 * kVectorSize); // a column may repeat
    const auto cols = Concatenate(ReadWithThreads(scan, 3), 3);
    ExpectColumnsEqual({cols[0]}, {b.model.cols[5]});
    ExpectColumnsEqual({cols[1]}, {b.model.cols[1]});
    ExpectColumnsEqual({cols[2]}, {b.model.cols[5]});
}

TEST(MorselScan, ASnapshotIsUnaffectedByLaterAppends) {
    Built b = BuildTable(5000, kSmallGroup, 17);
    const auto snap = b.table->Snapshot();
    MorselScan scan(snap, test::AllColumns(*snap), {}, kVectorSize);
    test::Rng rng(18);
    TableModel extra(b.table->schema().size());
    b.table->Append(RandomChunk(b.table->schema(), rng, 1000, extra));
    ExpectColumnsEqual(Concatenate(ReadWithThreads(scan, 4), scan.types().size()), b.model.cols);
}

// ---------------------------------------------------------------- adaptive morsel size

namespace {

// Runs with the built-in morsel size even when the test run set one (CDB_MORSEL_ROWS in the
// -parallel presets), restoring that setting afterwards.
class BuiltInMorselSize {
  public:
    BuiltInMorselSize() : saved_(MorselScan::DefaultMorselRows()) {
        MorselScan::SetDefaultMorselRows(0);
    }
    ~BuiltInMorselSize() {
        MorselScan::SetDefaultMorselRows(saved_ == MorselScan::kDefaultMorselRows ? 0 : saved_);
    }
    BuiltInMorselSize(const BuiltInMorselSize&) = delete;
    BuiltInMorselSize& operator=(const BuiltInMorselSize&) = delete;

  private:
    idx_t saved_;
};

// The rule, written out independently of the implementation.
idx_t ExpectedMorselRows(idx_t rows, size_t threads) {
    if (threads <= 1) {
        return MorselScan::kDefaultMorselRows;
    }
    const idx_t even = AlignUp(rows / (threads * MorselScan::kMorselsPerThread), kVectorSize);
    return std::clamp<idx_t>(even, kVectorSize, MorselScan::kDefaultMorselRows);
}

idx_t ExpectedMorselCount(const TableSnapshot& snap, idx_t morsel_rows) {
    idx_t n = 0;
    for (idx_t g = 0; g < snap.row_group_count(); g++) {
        n += (snap.row_group(g).count() + morsel_rows - 1) / morsel_rows;
    }
    return n;
}

} // namespace

TEST(MorselScan, ASmallTableIsCutIntoMorselsForEveryThread) {
    const BuiltInMorselSize built_in;
    const Built b = BuildTable(15000, kRowGroupSize, 31);
    const auto snap = b.table->Snapshot();
    {
        MorselScan one(snap, {0});
        EXPECT_EQ(one.MorselCount(), 1U) << "one thread: the built-in size, a single morsel";
        MorselScan one_explicit(snap, {0}, {}, 0, 1);
        EXPECT_EQ(one_explicit.MorselCount(), 1U);
    }
    for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
        MorselScan scan(snap, test::AllColumns(*snap), {}, 0, threads);
        EXPECT_EQ(scan.MorselCount(), 8U) << threads << " threads: 15,000 rows in 2048-row morsels";
        EXPECT_EQ(scan.RowCount(), 15000U);
        ExpectColumnsEqual(Concatenate(ReadWithThreads(scan, threads), scan.types().size()),
                           b.model.cols);
    }
}

TEST(MorselScan, TheMorselSizeFollowsTheRowsAndTheThreads) {
    const BuiltInMorselSize built_in;
    for (const idx_t total : {idx_t{1}, idx_t{2048}, idx_t{5000}, idx_t{100000}, idx_t{300000}}) {
        const Built b = BuildTable(total, kRowGroupSize, total + 3);
        const auto snap = b.table->Snapshot();
        for (const size_t threads :
             {size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{8}, size_t{16}, size_t{64}}) {
            const idx_t rows = ExpectedMorselRows(total, threads);
            MorselScan scan(snap, {0}, {}, 0, threads);
            ScanMorsel m;
            idx_t covered = 0, widest = 0, count = 0;
            while (scan.Next(m)) {
                covered += m.count;
                widest = std::max(widest, m.count);
                count++;
            }
            EXPECT_EQ(covered, total) << total << " rows " << threads << " threads";
            EXPECT_LE(widest, rows);
            EXPECT_EQ(count, ExpectedMorselCount(*snap, rows)) << total << " rows, " << threads;
            // rounding up to whole vectors costs at most a factor of two: at least two morsels per
            // thread unless the table has fewer vectors than that
            const idx_t vectors = (total + kVectorSize - 1) / kVectorSize;
            if (threads > 1) {
                EXPECT_GE(count, std::min<idx_t>(vectors, 2 * threads))
                    << total << " rows, " << threads << " threads";
            }
        }
    }
}

TEST(MorselScan, AnExplicitMorselSizeIsNeverAdapted) {
    const BuiltInMorselSize built_in;
    const Built b = BuildTable(15000, kRowGroupSize, 33);
    const auto snap = b.table->Snapshot();
    MorselScan explicit_size(snap, {0}, {}, 8 * kVectorSize, 16);
    EXPECT_EQ(explicit_size.MorselCount(), 1U) << "the caller asked for 8-vector morsels";
    MorselScan two(snap, {0}, {}, 2 * kVectorSize, 16);
    EXPECT_EQ(two.MorselCount(), 4U);

    // a default somebody set (a test, or CDB_MORSEL_ROWS) is also respected
    MorselScan::SetDefaultMorselRows(4 * kVectorSize);
    MorselScan from_default(snap, {0}, {}, 0, 16);
    EXPECT_EQ(from_default.MorselCount(), 2U);
    MorselScan::SetDefaultMorselRows(0);
}

TEST(MorselScan, TheSizeIsChosenFromTheRowsThatSurvivePruning) {
    const BuiltInMorselSize built_in;
    // Four full row groups with ascending ids; a filter keeps only the last one.
    Table table("t", {{"id", LogicalType::BigInt()}});
    for (idx_t at = 0; at < 4 * kRowGroupSize; at += kVectorSize) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::BigInt()}, kVectorSize);
        for (idx_t i = 0; i < kVectorSize; i++) {
            chunk.SetValue(0, i, Value::BigInt(static_cast<int64_t>(at + i)));
        }
        chunk.SetCardinality(kVectorSize);
        table.Append(chunk);
    }
    const auto snap = table.Snapshot();
    ASSERT_EQ(snap->row_group_count(), 4U);
    const std::vector<TableFilter> keep_last = {
        {0, CompareOp::Ge, Value::BigInt(static_cast<int64_t>(3 * kRowGroupSize))}};
    const auto widest = [](MorselScan& scan) {
        idx_t w = 0;
        ScanMorsel m;
        while (scan.Next(m)) {
            w = std::max(w, m.count);
        }
        return w;
    };
    MorselScan all(snap, {0}, {}, 0, 16);
    MorselScan pruned(snap, {0}, keep_last, 0, 16);
    EXPECT_EQ(pruned.row_groups_scanned(), 1U);
    EXPECT_EQ(pruned.RowCount(), kRowGroupSize);
    // 491,520 rows over 16 threads x 4 morsels -> 8192-row morsels; the 122,880 rows that survive
    // pruning -> 2048-row morsels. Sizing from the rows that are actually read keeps the
    // pruned scan fine-grained.
    const idx_t widest_all = widest(all), widest_pruned = widest(pruned); // drains both cursors
    EXPECT_EQ(widest_all, ExpectedMorselRows(4 * kRowGroupSize, 16));
    EXPECT_EQ(widest_pruned, ExpectedMorselRows(kRowGroupSize, 16));
    EXPECT_EQ(widest_all, 8192U);
    EXPECT_EQ(widest_pruned, 2048U);
    MorselScan all_again(snap, {0}, {}, 0, 16), pruned_again(snap, {0}, keep_last, 0, 16);
    EXPECT_EQ(all_again.MorselCount(), ExpectedMorselCount(*snap, 8192));
    EXPECT_EQ(pruned_again.MorselCount(), kRowGroupSize / 2048);
}

} // namespace cdb
