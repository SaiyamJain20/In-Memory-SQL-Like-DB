#pragma once

#include "planner/logical_plan.h"

namespace cdb {

// Rule-based logical optimizer (cost-based join ordering and statistics arrive in Phase 8):
//
//  * Filter pushdown. WHERE conjuncts move as close to the scans as their meaning allows: through
//    projections (by substitution), group-by columns, ordering and DISTINCT, into the side of an
//    inner join they reference, and - for outer joins - only to the preserved side (and ON-clause
//    conjuncts that mention only the other side). Conjuncts of the form `column <op> constant`
//    directly above a scan also become zone-map pruning hints on the scan.
//  * Join ordering. A tree of inner/cross joins (`FROM a, b, c WHERE ...`) is flattened and rebuilt
//    left-deep: start from the largest relation (the probe side), then repeatedly join the
//    smallest relation that is connected to the chosen ones by an equality predicate, so the build
//    sides stay small and no cross products arise where the predicates avoid them. Row estimates
//    come from table sizes and fixed predicate selectivities.
//  * Column pruning. Scans read, and operators carry, only the columns that are used downstream.
//
// The result is a plan with the same output columns (names, types, order) as the input.
LogicalPtr Optimize(LogicalPtr plan);

} // namespace cdb
