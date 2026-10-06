#pragma once

#include "planner/logical_plan.h"

namespace cdb {

// Logical optimizer:
//
//  * Filter pushdown. WHERE conjuncts move as close to the scans as their meaning allows: through
//    projections (by substitution), group-by columns, ordering and DISTINCT, into the side of an
//    inner join they reference, and - for outer joins - only to the preserved side (and ON-clause
//    conjuncts that mention only the other side). Conjuncts of the form `column <op> constant`
//    directly above a scan also become zone-map pruning hints on the scan. Semi and anti joins
//    (unnested subqueries) sink to the side of an inner join they mention.
//  * Join ordering. A tree of inner/cross joins (`FROM a, b, c WHERE ...`) is flattened and
//    rebuilt as the cheapest tree by estimated cost (planner/join_order.h): a dynamic program
//    over the subsets of up to 12 relations, bushy trees included, never a cross product where
//    the predicates avoid one, the larger input of each join probing and the smaller one built.
//    Row counts come from table statistics (planner/cardinality.h: min / max, distinct counts
//    from per-segment HyperLogLog sketches, NULL fractions); more relations fall back to a greedy
//    left-deep order.
//  * Column pruning. Scans read, and operators carry, only the columns that are used downstream.
//
// The result is a plan with the same output columns (names, types, order) as the input.
LogicalPtr Optimize(LogicalPtr plan);

} // namespace cdb
