#pragma once

// Helpers for storage tests: build random DataChunks while recording a plain row-model of what
// was appended, and read a table back through TableScan into the same shape for comparison.

#include "storage/encoding.h"
#include "storage/table.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cdb::test {

// Turns compression on or off for the lifetime of the object (it is a process-wide switch, so tests
// that rely on a particular segment layout say so explicitly).
class ScopedCompression {
  public:
    explicit ScopedCompression(bool enabled) : previous_(CompressionEnabled()) {
        SetCompressionEnabled(enabled);
    }
    ~ScopedCompression() { SetCompressionEnabled(previous_); }
    ScopedCompression(const ScopedCompression&) = delete;
    ScopedCompression& operator=(const ScopedCompression&) = delete;

  private:
    bool previous_;
};

// Column-major model of a table's contents.
struct TableModel {
    std::vector<std::vector<Value>> cols;
    explicit TableModel(size_t ncols) : cols(ncols) {}
    idx_t rows() const { return cols.empty() ? 0 : cols[0].size(); }
};

inline std::vector<ColumnDefinition> AllTypesSchema() {
    return {{"b", LogicalType::Boolean()}, {"i", LogicalType::Integer()},
            {"l", LogicalType::BigInt()},  {"d", LogicalType::Double()},
            {"dt", LogicalType::Date()},   {"s", LogicalType::Varchar()}};
}

// Builds a chunk of `n` random rows (<= kVectorSize) and appends them to `model`.
inline DataChunk RandomChunk(const std::vector<ColumnDefinition>& schema, Rng& rng, idx_t n,
                             TableModel& model, double null_prob = 0.2) {
    std::vector<LogicalType> types;
    for (const auto& c : schema)
        types.push_back(c.type);
    DataChunk chunk;
    chunk.Initialize(types);
    for (idx_t c = 0; c < schema.size(); c++) {
        for (idx_t r = 0; r < n; r++) {
            Value v = RandomValue(rng, schema[c].type, null_prob);
            chunk.SetValue(c, r, v);
            model.cols[c].push_back(std::move(v));
        }
    }
    chunk.SetCardinality(n);
    return chunk;
}

// Reads the projected columns of every row a scan produces, in order.
inline std::vector<std::vector<Value>> ScanAll(TableScan& scan) {
    DataChunk chunk;
    chunk.Initialize(scan.types());
    std::vector<std::vector<Value>> out(scan.types().size());
    while (scan.Next(chunk)) {
        chunk.Verify();
        for (idx_t c = 0; c < out.size(); c++) {
            for (idx_t r = 0; r < chunk.size(); r++)
                out[c].push_back(chunk.GetValue(c, r));
        }
    }
    return out;
}

inline std::vector<idx_t> AllColumns(const TableSnapshot& snap) {
    std::vector<idx_t> ids;
    for (idx_t i = 0; i < snap.schema().size(); i++)
        ids.push_back(i);
    return ids;
}

inline void ExpectColumnsEqual(const std::vector<std::vector<Value>>& got,
                               const std::vector<std::vector<Value>>& expect,
                               const std::string& ctx = "") {
    ASSERT_EQ(got.size(), expect.size()) << ctx;
    for (size_t c = 0; c < got.size(); c++) {
        ASSERT_EQ(got[c].size(), expect[c].size()) << ctx << " column " << c;
        for (size_t r = 0; r < got[c].size(); r++) {
            ASSERT_TRUE(BitIdentical(got[c][r], expect[c][r]))
                << ctx << " column " << c << " row " << r << ": got " << got[c][r].ToString()
                << " expected " << expect[c][r].ToString();
        }
    }
}

// Every row of a segment, vector by vector (raw and encoded alike).
inline std::vector<Value> ScanSegment(const ColumnSegment& seg) {
    std::vector<Value> out;
    Vector v(seg.type(), kVectorSize);
    for (idx_t at = 0; at < seg.count(); at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, seg.count() - at);
        seg.Scan(at, n, v);
        v.Verify(n);
        for (idx_t i = 0; i < n; i++)
            out.push_back(v.GetValue(i));
    }
    return out;
}

// Two segments hold the same bits, the same statistics, and are stored the same way.
inline void ExpectSameSegment(const ColumnSegment& got, const ColumnSegment& want,
                              const std::string& ctx) {
    ASSERT_EQ(got.type(), want.type()) << ctx;
    ASSERT_EQ(got.count(), want.count()) << ctx;
    EXPECT_EQ(got.stats().null_count, want.stats().null_count) << ctx;
    EXPECT_EQ(got.stats().min.has_value(), want.stats().min.has_value()) << ctx;
    EXPECT_EQ(got.stats().max.has_value(), want.stats().max.has_value()) << ctx;
    if (got.stats().min && want.stats().min) {
        EXPECT_TRUE(BitIdentical(*got.stats().min, *want.stats().min)) << ctx << ": min";
        EXPECT_TRUE(BitIdentical(*got.stats().max, *want.stats().max)) << ctx << ": max";
    }
    // the distinct-value sketch is part of the statistics: kept bit for bit, or absent in both
    ASSERT_EQ(got.stats().distinct == nullptr, want.stats().distinct == nullptr)
        << ctx << ": sketch";
    if (got.stats().distinct != nullptr) {
        EXPECT_TRUE(*got.stats().distinct == *want.stats().distinct) << ctx << ": sketch";
    }
    ASSERT_EQ(got.encoded(), want.encoded()) << ctx;
    if (got.encoded()) {
        EXPECT_EQ(got.encoding()->kind(), want.encoding()->kind()) << ctx;
    }
    const std::vector<Value> a = ScanSegment(got), b = ScanSegment(want);
    for (size_t i = 0; i < a.size(); i++) {
        ASSERT_TRUE(BitIdentical(a[i], b[i]))
            << ctx << ": row " << i << " got " << a[i].ToString() << " want " << b[i].ToString();
    }
}

// Does `value <op> constant` hold under SQL semantics (NULL never satisfies)?
inline bool Satisfies(const Value& value, CompareOp op, const Value& constant) {
    if (value.IsNull() || constant.IsNull())
        return false;
    const int c = Value::Compare(value, constant);
    switch (op) {
    case CompareOp::Eq:
        return c == 0;
    case CompareOp::Ne:
        return c != 0;
    case CompareOp::Lt:
        return c < 0;
    case CompareOp::Le:
        return c <= 0;
    case CompareOp::Gt:
        return c > 0;
    case CompareOp::Ge:
        return c >= 0;
    }
    return false;
}

inline const std::vector<CompareOp>& AllOps() {
    static const std::vector<CompareOp> kOps = {CompareOp::Eq, CompareOp::Ne, CompareOp::Lt,
                                                CompareOp::Le, CompareOp::Gt, CompareOp::Ge};
    return kOps;
}

} // namespace cdb::test
