#pragma once

#include "execution/pipeline.h"
#include "planner/logical_plan.h"

#include <memory>

namespace cdb {

// Turns an (optimized) logical plan into pipelines of physical operators.
//
//   scan/values              -> a pipeline source
//   filter, projection, limit -> streaming operators of the current pipeline
//   aggregate, distinct, order, top-N -> pipeline breakers: the child runs as its own pipeline
//                               ending in the breaker, which then is the source of the parent's
//   join                     -> the left (probe) input continues the current pipeline; the right
//                               (build) input is a separate pipeline ending in the join's build
//                               sink, which must finish first. Equality conjuncts of the join
//                               condition become hash keys, the rest a residual predicate; a
//                               condition with no equalities (or none at all) gives a nested-loop
//                               join. RIGHT joins run as LEFT joins with the sides swapped (plus
//                               a projection restoring the column order); FULL joins are not
//                               supported yet.
//   LIMIT over ORDER BY      -> top-N
//
// Every table is read from a snapshot taken here, so all scans of one table in a query agree.
//
// PlanSelect's root is a PhysicalResultCollector; PlanInsert's is a PhysicalInsert (its child must
// already produce the target table's column types).
std::unique_ptr<PhysicalPlan> PlanSelect(const LogicalOperator& root);
std::unique_ptr<PhysicalPlan> PlanInsert(const LogicalInsert& insert);

} // namespace cdb
