#include "planner/join_order.h"

#include "common/assert.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace cdb {

namespace {

// The edges folded into independent factors: each is the selectivity of a set of relations that
// applies once all of them are in the join.
struct Factor {
    uint64_t mask;
    double selectivity;
};

std::vector<Factor> FoldEdges(const std::vector<double>& rows, const std::vector<JoinEdge>& edges) {
    std::vector<Factor> factors;
    struct Key {
        double combinations = 1;
    };
    std::map<std::pair<int, int>, Key> composite;
    for (const JoinEdge& e : edges) {
        if (e.equality && e.a >= 0 && e.b >= 0 && e.a != e.b) {
            composite[{std::min(e.a, e.b), std::max(e.a, e.b)}].combinations *=
                std::max({1.0, e.distinct_a, e.distinct_b});
        } else {
            factors.push_back({e.relations, std::min(1.0, std::max(0.0, e.selectivity))});
        }
    }
    for (const auto& [pair, key] : composite) {
        const double larger =
            std::max(rows[static_cast<size_t>(pair.first)], rows[static_cast<size_t>(pair.second)]);
        const double combinations = std::min(key.combinations, std::max(1.0, larger));
        factors.push_back(
            {(uint64_t{1} << pair.first) | (uint64_t{1} << pair.second), 1.0 / combinations});
    }
    return factors;
}

double SetRows(const std::vector<double>& rows, const std::vector<Factor>& factors, uint64_t set) {
    double out = 1;
    for (uint64_t rest = set; rest != 0; rest &= rest - 1) {
        out *= std::max(1.0, rows[static_cast<size_t>(std::countr_zero(rest))]);
    }
    for (const Factor& f : factors) {
        if ((f.mask & ~set) == 0) {
            out *= f.selectivity;
        }
    }
    return std::max(1.0, out);
}

// Is there a predicate joining something in `a` with something in `b` that is fully inside a|b?
bool Connected(const std::vector<JoinEdge>& edges, uint64_t a, uint64_t b) {
    const uint64_t all = a | b;
    for (const JoinEdge& e : edges) {
        if ((e.relations & ~all) == 0 && (e.relations & a) != 0 && (e.relations & b) != 0) {
            return true;
        }
    }
    return false;
}

// Adds a join of the subtrees at nodes `x` and `y`; the larger input probes (is the left child).
int AddJoin(JoinTree& tree, int x, int y, double rows) {
    const JoinTree::Node nx = tree.nodes[static_cast<size_t>(x)];
    const JoinTree::Node ny = tree.nodes[static_cast<size_t>(y)];
    // The larger input probes; estimates that agree to within rounding are a tie, and then the
    // relation that comes first in the query probes (so that floating-point noise decides nothing).
    const double larger = std::max(nx.rows, ny.rows);
    const bool tie = std::fabs(nx.rows - ny.rows) <= 1e-9 * larger;
    const bool x_probes = tie ? nx.set < ny.set : nx.rows > ny.rows;
    JoinTree::Node n;
    n.left = x_probes ? x : y;
    n.right = x_probes ? y : x;
    n.set = nx.set | ny.set;
    n.rows = rows;
    n.cost = nx.cost + ny.cost + JoinCost(nx.rows, ny.rows, rows);
    tree.nodes.push_back(n);
    return static_cast<int>(tree.nodes.size()) - 1;
}

int AddLeaf(JoinTree& tree, int relation, double rows) {
    JoinTree::Node n;
    n.relation = relation;
    n.set = uint64_t{1} << relation;
    n.rows = std::max(1.0, rows);
    tree.nodes.push_back(n);
    return static_cast<int>(tree.nodes.size()) - 1;
}

JoinTree Exhaustive(const std::vector<double>& rows, const std::vector<JoinEdge>& edges,
                    const std::vector<Factor>& factors) {
    const size_t n = rows.size();
    const uint64_t full = (uint64_t{1} << n) - 1;
    // Two tables over the subsets. `connected[s]` is the cost of the cheapest tree that joins the
    // relations of `s` only through predicates (-1 if there is none: the set falls apart into
    // parts no predicate links). `any[s]` is the cheapest tree at all, a cross product if it must:
    // used for such a set only, so a cross product never appears inside a plan for a connected
    // join graph - a set a predicate cannot join is never combined with another by a join that a
    // predicate could have made.
    std::vector<double> connected(full + 1, -1.0), any(full + 1, -1.0), card(full + 1, 0.0);
    std::vector<uint64_t> split(full + 1,
                                0); // the part of the best split that holds the lowest relation
    for (size_t i = 0; i < n; i++) {
        const uint64_t s = uint64_t{1} << i;
        connected[s] = any[s] = 0;
        card[s] = std::max(1.0, rows[i]);
    }
    for (uint64_t set = 1; set <= full; set++) {
        if (std::popcount(set) < 2) {
            continue;
        }
        card[set] = SetRows(rows, factors, set);
        const uint64_t low = set & (~set + 1);
        double best_connected = -1, best_any = -1;
        uint64_t split_connected = 0, split_any = 0;
        // every split of `set` into two non-empty parts, once: `part` holds the lowest relation
        for (uint64_t part = (set - 1) & set; part != 0; part = (part - 1) & set) {
            if ((part & low) == 0) {
                continue;
            }
            const uint64_t other = set ^ part;
            const double join = JoinCost(card[part], card[other], card[set]);
            if (connected[part] >= 0 && connected[other] >= 0 && Connected(edges, part, other)) {
                const double c = connected[part] + connected[other] + join;
                if (best_connected < 0 || c < best_connected * (1 - 1e-12)) {
                    best_connected = c;
                    split_connected = part;
                }
            }
            const double c = any[part] + any[other] + join;
            if (best_any < 0 || c < best_any * (1 - 1e-12)) {
                best_any = c;
                split_any = part;
            }
        }
        if (best_connected >= 0) {
            connected[set] = any[set] = best_connected;
            split[set] = split_connected;
        } else {
            any[set] = best_any;
            split[set] = split_any;
        }
    }
    JoinTree tree;
    // build the tree of the best splits, depth first
    struct Builder {
        const std::vector<double>& rows;
        const std::vector<uint64_t>& split;
        const std::vector<double>& card;
        JoinTree& tree;
        int Build(uint64_t set) {
            if (std::popcount(set) == 1) {
                const int relation = std::countr_zero(set);
                return AddLeaf(tree, relation, rows[static_cast<size_t>(relation)]);
            }
            const uint64_t part = split[set];
            const int x = Build(part);
            const int y = Build(set ^ part);
            return AddJoin(tree, x, y, card[set]);
        }
    } builder{rows, split, card, tree};
    tree.root = builder.Build(full);
    return tree;
}

// Left-deep: start from the largest relation, then repeatedly add the relation that gives the
// smallest intermediate result among those joined to it by a predicate (any, if none is).
JoinTree Greedy(const std::vector<double>& rows, const std::vector<JoinEdge>& edges,
                const std::vector<Factor>& factors) {
    const size_t n = rows.size();
    JoinTree tree;
    size_t start = 0;
    for (size_t i = 1; i < n; i++) {
        if (rows[i] > rows[start]) {
            start = i;
        }
    }
    uint64_t chosen = uint64_t{1} << start;
    int current = AddLeaf(tree, static_cast<int>(start), rows[start]);
    while (std::popcount(chosen) < static_cast<int>(n)) {
        bool best_connected = false;
        double best_rows = 0;
        size_t best = n;
        for (size_t i = 0; i < n; i++) {
            const uint64_t bit = uint64_t{1} << i;
            if ((chosen & bit) != 0) {
                continue;
            }
            const bool connected = Connected(edges, chosen, bit);
            const double result = SetRows(rows, factors, chosen | bit);
            if (best == n || (connected && !best_connected) ||
                (connected == best_connected && result < best_rows)) {
                best = i;
                best_connected = connected;
                best_rows = result;
            }
        }
        const int leaf = AddLeaf(tree, static_cast<int>(best), rows[best]);
        current = AddJoin(tree, current, leaf, best_rows);
        chosen |= uint64_t{1} << best;
    }
    tree.root = current;
    return tree;
}

} // namespace

double JoinCost(double left_rows, double right_rows, double out_rows) {
    const double build = std::min(left_rows, right_rows);
    const double probe = std::max(left_rows, right_rows);
    return out_rows + 2.0 * build + probe;
}

double JoinSetRows(const std::vector<double>& rows, const std::vector<JoinEdge>& edges,
                   uint64_t set) {
    return SetRows(rows, FoldEdges(rows, edges), set);
}

JoinTree ChooseJoinOrder(const std::vector<double>& rows, const std::vector<JoinEdge>& edges) {
    CDB_CHECK(!rows.empty() && rows.size() <= kMaxJoinRelations);
    const std::vector<Factor> factors = FoldEdges(rows, edges);
    if (rows.size() == 1) {
        JoinTree tree;
        tree.root = AddLeaf(tree, 0, rows[0]);
        return tree;
    }
    return rows.size() <= kMaxExhaustiveRelations ? Exhaustive(rows, edges, factors)
                                                  : Greedy(rows, edges, factors);
}

JoinTree ChooseJoinOrderGreedy(const std::vector<double>& rows,
                               const std::vector<JoinEdge>& edges) {
    CDB_CHECK(!rows.empty() && rows.size() <= kMaxJoinRelations);
    if (rows.size() == 1) {
        return ChooseJoinOrder(rows, edges);
    }
    return Greedy(rows, edges, FoldEdges(rows, edges));
}

double CostOfTree(const JoinTree& tree) {
    return tree.root < 0 ? 0 : tree.nodes[static_cast<size_t>(tree.root)].cost;
}

} // namespace cdb
