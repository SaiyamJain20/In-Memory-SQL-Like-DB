#pragma once

#include <cstdint>
#include <vector>

namespace cdb {

// Choosing the order in which a set of relations is joined, from cardinality estimates alone (the
// join graph, not the operators: the optimizer maps its plan onto this and back).
//
// The cost model counts rows through each hash join: every output row, twice the rows hashed into
// the build side (the smaller input) and once the rows that probe it. The size of the result of
// joining a set of relations does not depend on the order: it is the product of the relations'
// rows times the selectivity of every predicate whose relations are all in the set, so plans for
// the same set are comparable and a dynamic program over the subsets finds the cheapest of all
// bushy trees.

// A join predicate. It mentions two or more relations (bit i of `relations` is relation i).
struct JoinEdge {
    uint64_t relations = 0;
    // An equality between a column of relation `a` and one of relation `b`: then the selectivity
    // follows from the numbers of distinct values (1 / the larger one; several equalities between
    // the same two relations form one composite key, which cannot have more combinations than
    // the larger relation has rows). Otherwise `selectivity` is used as it is.
    bool equality = false;
    int a = -1, b = -1;
    double distinct_a = 1, distinct_b = 1;
    double selectivity = 0.3;
};

// A binary tree over the relations. A join's left child is the probe side (the larger input), its
// right child the build side.
struct JoinTree {
    struct Node {
        int relation = -1; // a leaf: the relation; else -1
        int left = -1;     // indexes into `nodes`
        int right = -1;
        uint64_t set = 0; // the relations below
        double rows = 0;  // estimated rows of the result
        double cost = 0;  // total cost of the subtree
    };
    std::vector<Node> nodes;
    int root = -1;
};

// Up to this many relations are ordered exhaustively (3^n splits); more fall back to a greedy
// left-deep order.
inline constexpr size_t kMaxExhaustiveRelations = 12;
// At most this many (bitmasks are 64 bits wide).
inline constexpr size_t kMaxJoinRelations = 60;

// The estimated rows of joining the relations in `set`: product of their rows times the
// selectivities of the edges inside it (never below 1).
double JoinSetRows(const std::vector<double>& rows, const std::vector<JoinEdge>& edges,
                   uint64_t set);

// The cost of one join of inputs with the given row counts that produces `out` rows.
double JoinCost(double left_rows, double right_rows, double out_rows);

// The cheapest tree: every relation joined, cross products only where the join graph forces them.
// `rows[i]` is the (filtered) row count of relation i; 1 <= rows.size() <= kMaxJoinRelations.
JoinTree ChooseJoinOrder(const std::vector<double>& rows, const std::vector<JoinEdge>& edges);

// The greedy left-deep order that ChooseJoinOrder falls back to (exposed to be compared with the
// exhaustive one): start from the largest relation, then add the relation that gives the smallest
// intermediate result among those joined to the current ones by a predicate.
JoinTree ChooseJoinOrderGreedy(const std::vector<double>& rows, const std::vector<JoinEdge>& edges);

// The same cost model applied to a given tree, for comparing plans (and for tests).
double CostOfTree(const JoinTree& tree);

} // namespace cdb
