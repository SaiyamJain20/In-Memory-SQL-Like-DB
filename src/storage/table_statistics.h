#pragma once

#include "storage/table.h"

#include <memory>
#include <optional>
#include <vector>

namespace cdb {

// What the optimizer knows about one column of a table: summed up from the zone maps and the
// distinct-value sketches of its segments.
struct ColumnStatistics {
    idx_t null_count = 0;
    // Bounds over the non-NULL values; absent if there are none, or if some segment has values
    // but no usable bounds (a VARCHAR segment with a string longer than the zone-map limit).
    std::optional<Value> min;
    std::optional<Value> max;
    // The estimated number of distinct non-NULL values: the sketches merged, 0 if every value is
    // NULL, never more than the number of non-NULL rows.
    double distinct = 0;
};

struct TableStatistics {
    idx_t row_count = 0;
    std::vector<ColumnStatistics> columns;

    double NullFraction(idx_t column) const {
        return row_count == 0 ? 0.0
                              : static_cast<double>(columns.at(column).null_count) /
                                    static_cast<double>(row_count);
    }
};

// Statistics of everything `snapshot` holds. Merges the sketches of the sealed segments; a segment
// without one (the frozen copy of an open tail) is read to build it.
std::shared_ptr<const TableStatistics> ComputeTableStatistics(const TableSnapshot& snapshot);

} // namespace cdb
