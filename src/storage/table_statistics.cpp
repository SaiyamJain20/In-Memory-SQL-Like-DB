#include "storage/table_statistics.h"

#include <algorithm>

namespace cdb {

namespace {

// The sketch of a segment that has none: its rows, read one vector at a time (a raw segment is
// read in place).
HyperLogLog SketchOf(const ColumnSegment& segment) {
    HyperLogLog sketch;
    if (!segment.encoded()) {
        AddToDistinctSketch(sketch, segment.type(), segment.raw_data(), segment.validity(),
                            segment.count());
        return sketch;
    }
    Vector out(segment.type());
    for (idx_t offset = 0; offset < segment.count(); offset += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, segment.count() - offset);
        segment.Scan(offset, n, out);
        AddToDistinctSketch(sketch, segment.type(), out.FlatBytes(), out.Validity(), n);
    }
    return sketch;
}

} // namespace

std::shared_ptr<const TableStatistics> ComputeTableStatistics(const TableSnapshot& snapshot) {
    auto result = std::make_shared<TableStatistics>();
    result->row_count = snapshot.row_count();
    const size_t columns = snapshot.schema().size();
    result->columns.resize(columns);
    for (size_t c = 0; c < columns; c++) {
        ColumnStatistics& out = result->columns[c];
        HyperLogLog merged;
        bool bounds_lost = false;
        for (idx_t g = 0; g < snapshot.row_group_count(); g++) {
            const ColumnSegment& segment = snapshot.row_group(g).column(c);
            const ColumnStats& st = segment.stats();
            out.null_count += st.null_count;
            if (st.AllNull()) {
                continue;
            }
            if (st.min && st.max) {
                if (!out.min || Value::Compare(*st.min, *out.min) < 0) {
                    out.min = st.min;
                }
                if (!out.max || Value::Compare(*st.max, *out.max) > 0) {
                    out.max = st.max;
                }
            } else {
                bounds_lost = true;
            }
            if (st.distinct != nullptr) {
                merged.Merge(*st.distinct);
            } else {
                merged.Merge(SketchOf(segment));
            }
        }
        if (bounds_lost) {
            out.min.reset();
            out.max.reset();
        }
        const double non_null = static_cast<double>(result->row_count - out.null_count);
        out.distinct = std::min(merged.Estimate(), non_null);
        if (non_null > 0) {
            out.distinct = std::max(out.distinct, 1.0);
        }
    }
    return result;
}

} // namespace cdb
