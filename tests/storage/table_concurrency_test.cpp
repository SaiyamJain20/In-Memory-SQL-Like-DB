// Concurrency tests for Table: snapshot-isolated readers racing a writer. Meaningful chiefly
// under ThreadSanitizer (`ctest --preset tsan`), which flags any unsynchronised access; the
// assertions additionally verify that every reader observes a *consistent prefix* of the table.

#include "storage/table.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace cdb {

namespace {

// Row i is (id = i, tag = "row-<i>" padded to > 12 bytes, value = i * 3 or NULL if i % 7 == 0).
Value TagFor(int64_t i) {
    return Value::Varchar("row-" + std::to_string(i) + "-padding-to-force-the-heap");
}

} // namespace

TEST(TableConcurrency, ReadersSeeConsistentPrefixesWhileAWriterAppends) {
    const std::vector<ColumnDefinition> schema = {{"id", LogicalType::BigInt()},
                                                  {"tag", LogicalType::Varchar()},
                                                  {"v", LogicalType::BigInt()}};
    Table table("t", schema, 2 * kVectorSize); // small groups: many seals during the test

    constexpr int kChunks = 120;
    constexpr int kRowsPerChunk = 700;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> scans{0}, rows_checked{0};
    std::atomic<bool> failed{false};

    auto reader = [&] {
        while (!done.load() && !failed.load()) {
            auto snap = table.Snapshot();
            TableScan scan(snap, {0, 1, 2});
            DataChunk chunk;
            chunk.Initialize(scan.types());
            int64_t expect_id = 0;
            while (scan.Next(chunk)) {
                for (idx_t r = 0; r < chunk.size(); r++) {
                    const Value id = chunk.GetValue(0, r);
                    const Value tag = chunk.GetValue(1, r);
                    const Value v = chunk.GetValue(2, r);
                    const bool ok =
                        id.GetBigInt() == expect_id && tag == TagFor(expect_id) &&
                        (expect_id % 7 == 0 ? v.IsNull() : v.GetBigInt() == expect_id * 3);
                    if (!ok) {
                        failed = true;
                        ADD_FAILURE()
                            << "inconsistent row " << expect_id << ": id=" << id.ToString()
                            << " tag=" << tag.ToString() << " v=" << v.ToString();
                        return;
                    }
                    expect_id++;
                }
            }
            if (static_cast<idx_t>(expect_id) != snap->row_count()) {
                failed = true;
                ADD_FAILURE() << "scan returned " << expect_id << " rows, snapshot says "
                              << snap->row_count();
                return;
            }
            scans++;
            rows_checked += static_cast<uint64_t>(expect_id);
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; i++)
        readers.emplace_back(reader);

    DataChunk chunk;
    chunk.Initialize({LogicalType::BigInt(), LogicalType::Varchar(), LogicalType::BigInt()});
    int64_t next = 0;
    for (int c = 0; c < kChunks; c++) {
        chunk.Reset();
        for (int r = 0; r < kRowsPerChunk; r++, next++) {
            chunk.SetValue(0, r, Value::BigInt(next));
            chunk.SetValue(1, r, TagFor(next));
            chunk.SetValue(
                2, r, next % 7 == 0 ? Value::Null(LogicalType::BigInt()) : Value::BigInt(next * 3));
        }
        chunk.SetCardinality(kRowsPerChunk);
        table.Append(chunk);
        if (c % 10 == 0)
            std::this_thread::yield();
    }
    done = true;
    for (auto& t : readers)
        t.join();

    EXPECT_FALSE(failed.load());
    EXPECT_EQ(table.RowCount(), static_cast<idx_t>(kChunks) * kRowsPerChunk);
    EXPECT_GT(scans.load(), 0u);
    // After the writer stops, a fresh snapshot must see everything.
    auto final_snap = table.Snapshot();
    EXPECT_EQ(final_snap->row_count(), static_cast<idx_t>(kChunks) * kRowsPerChunk);
}

TEST(TableConcurrency, ManyReadersScanTheSameSnapshotInParallel) {
    test::Rng rng(1);
    Table table("t", test::AllTypesSchema(), 2 * kVectorSize);
    test::TableModel model(table.schema().size());
    for (int i = 0; i < 30; i++) {
        table.Append(test::RandomChunk(table.schema(), rng, 700, model));
    }
    auto snap = table.Snapshot();

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&] {
            for (int rep = 0; rep < 5; rep++) {
                TableScan scan(snap, test::AllColumns(*snap));
                auto got = test::ScanAll(scan);
                for (size_t c = 0; c < got.size(); c++) {
                    if (got[c].size() != model.cols[c].size()) {
                        mismatches++;
                        continue;
                    }
                    for (size_t r = 0; r < got[c].size(); r++) {
                        if (!test::BitIdentical(got[c][r], model.cols[c][r]))
                            mismatches++;
                    }
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(TableConcurrency, ConcurrentSnapshotsOfTheOpenTailShareOneCopy) {
    Table table("t", {{"x", LogicalType::Integer()}}, 2 * kVectorSize);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (idx_t i = 0; i < 500; i++)
        chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(i)));
    chunk.SetCardinality(500);
    table.Append(chunk);

    std::vector<std::shared_ptr<const TableSnapshot>> snaps(8);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] { snaps[t] = table.Snapshot(); });
    }
    for (auto& t : threads)
        t.join();
    for (int t = 1; t < 8; t++) {
        EXPECT_EQ(snaps[t]->row_group_ptr(0).get(), snaps[0]->row_group_ptr(0).get());
    }
}

} // namespace cdb
