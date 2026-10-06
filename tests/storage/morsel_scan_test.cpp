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

} // namespace cdb
