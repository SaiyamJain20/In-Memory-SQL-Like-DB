#include "planner/join_order.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

JoinEdge Equality(int a, int b, double distinct_a, double distinct_b) {
    JoinEdge e;
    e.relations = (uint64_t{1} << a) | (uint64_t{1} << b);
    e.equality = true;
    e.a = a;
    e.b = b;
    e.distinct_a = distinct_a;
    e.distinct_b = distinct_b;
    return e;
}

JoinEdge Other(std::vector<int> relations, double selectivity) {
    JoinEdge e;
    for (const int r : relations) {
        e.relations |= uint64_t{1} << r;
    }
    e.selectivity = selectivity;
    return e;
}

// Every node: the rows are those of its set, the cost adds up, the probe side is not the smaller.
void CheckTree(const JoinTree& tree, const std::vector<double>& rows,
               const std::vector<JoinEdge>& edges, const std::string& what) {
    ASSERT_GE(tree.root, 0) << what;
    const uint64_t all = (uint64_t{1} << rows.size()) - 1;
    ASSERT_EQ(tree.nodes[static_cast<size_t>(tree.root)].set, all) << what;
    uint64_t leaves_seen = 0;
    for (const JoinTree::Node& n : tree.nodes) {
        if (n.relation >= 0) {
            EXPECT_EQ(n.set, uint64_t{1} << n.relation) << what;
            EXPECT_EQ((leaves_seen >> n.relation) & 1, 0U) << what << ": a relation twice";
            leaves_seen |= n.set;
            continue;
        }
        const JoinTree::Node& l = tree.nodes[static_cast<size_t>(n.left)];
        const JoinTree::Node& r = tree.nodes[static_cast<size_t>(n.right)];
        EXPECT_EQ(l.set & r.set, 0U) << what;
        EXPECT_EQ(l.set | r.set, n.set) << what;
        EXPECT_NEAR(n.rows, JoinSetRows(rows, edges, n.set), 1e-6 * n.rows) << what;
        EXPECT_NEAR(n.cost, l.cost + r.cost + JoinCost(l.rows, r.rows, n.rows), 1e-6 * n.cost)
            << what;
        EXPECT_GE(l.rows, r.rows * (1 - 1e-9)) << what << ": the larger input probes";
    }
    EXPECT_EQ(leaves_seen, all) << what;
}

bool HasCrossProduct(const JoinTree& tree, const std::vector<JoinEdge>& edges) {
    for (const JoinTree::Node& n : tree.nodes) {
        if (n.relation >= 0) {
            continue;
        }
        const uint64_t a = tree.nodes[static_cast<size_t>(n.left)].set;
        const uint64_t b = tree.nodes[static_cast<size_t>(n.right)].set;
        bool connected = false;
        for (const JoinEdge& e : edges) {
            connected = connected || ((e.relations & ~n.set) == 0 && (e.relations & a) != 0 &&
                                      (e.relations & b) != 0);
        }
        if (!connected) {
            return true;
        }
    }
    return false;
}

// A random join graph: a spanning tree (so it is connected) plus a few extra edges.
std::vector<JoinEdge> RandomEdges(Rng& rng, size_t n, const std::vector<double>& rows,
                                  bool connected = true) {
    std::vector<JoinEdge> edges;
    for (size_t i = 1; i < n; i++) {
        if (!connected && Chance(rng, 0.3)) {
            continue;
        }
        const int j = static_cast<int>(RandBelow(rng, i));
        edges.push_back(
            Equality(static_cast<int>(i), j,
                     1 + static_cast<double>(RandBelow(rng, static_cast<uint64_t>(rows[i]))),
                     1 + static_cast<double>(
                             RandBelow(rng, static_cast<uint64_t>(rows[static_cast<size_t>(j)])))));
    }
    for (size_t extra = RandBelow(rng, 3); extra > 0 && n >= 3; extra--) {
        const int a = static_cast<int>(RandBelow(rng, n)), b = static_cast<int>(RandBelow(rng, n));
        if (a == b) {
            continue;
        }
        edges.push_back(
            Chance(rng, 0.6)
                ? Equality(a, b, 1 + static_cast<double>(RandBelow(rng, 50)),
                           1 + static_cast<double>(RandBelow(rng, 50)))
                : Other({a, b}, 0.05 + 0.5 * static_cast<double>(RandBelow(rng, 100)) / 100));
    }
    if (n >= 3 && Chance(rng, 0.3)) { // a predicate over three relations
        edges.push_back(Other({0, 1, 2}, 0.2));
    }
    return edges;
}

std::vector<double> RandomRows(Rng& rng, size_t n) {
    std::vector<double> rows;
    for (size_t i = 0; i < n; i++) {
        rows.push_back(static_cast<double>(1 + RandBelow(rng, Chance(rng, 0.3) ? 20 : 100000)));
    }
    return rows;
}

// The cheapest tree over `set` among ALL binary trees, enumerated explicitly with no table of
// subproblems (the oracle for the dynamic program). A split must be joined by a predicate and have
// both parts joinable by predicates; a set for which there is no such split may be built from a
// cross product.
struct Brute {
    const std::vector<double>& rows;
    const std::vector<JoinEdge>& edges;

    bool Linked(uint64_t a, uint64_t b) const {
        for (const JoinEdge& e : edges) {
            if ((e.relations & ~(a | b)) == 0 && (e.relations & a) != 0 && (e.relations & b) != 0) {
                return true;
            }
        }
        return false;
    }

    // Can `set` be built from predicates alone?
    bool Joinable(uint64_t set) const {
        if (std::popcount(set) == 1) {
            return true;
        }
        const uint64_t low = set & (~set + 1);
        for (uint64_t part = (set - 1) & set; part != 0; part = (part - 1) & set) {
            if ((part & low) != 0 && Joinable(part) && Joinable(set ^ part) &&
                Linked(part, set ^ part)) {
                return true;
            }
        }
        return false;
    }

    // The cheapest tree over `set`, restricted to predicate joins if the set has one.
    double Cost(uint64_t set) const {
        if (std::popcount(set) == 1) {
            return 0;
        }
        const bool joinable = Joinable(set);
        const uint64_t low = set & (~set + 1);
        double best = -1;
        for (uint64_t part = (set - 1) & set; part != 0; part = (part - 1) & set) {
            if ((part & low) == 0) {
                continue;
            }
            const uint64_t other = set ^ part;
            if (joinable && !(Joinable(part) && Joinable(other) && Linked(part, other))) {
                continue;
            }
            const double c =
                Cost(part) + Cost(other) +
                JoinCost(JoinSetRows(rows, edges, part), JoinSetRows(rows, edges, other),
                         JoinSetRows(rows, edges, set));
            if (best < 0 || c < best) {
                best = c;
            }
        }
        return best;
    }
};

} // namespace

TEST(JoinCost, CountsOutputTwiceTheBuildSideAndTheProbeSide) {
    EXPECT_EQ(JoinCost(1000, 10, 50), 50 + 2 * 10 + 1000);
    EXPECT_EQ(JoinCost(10, 1000, 50), JoinCost(1000, 10, 50)) << "symmetric: the smaller builds";
}

TEST(JoinOrder, OneRelationIsALeaf) {
    const JoinTree t = ChooseJoinOrder({42}, {});
    ASSERT_EQ(t.nodes.size(), 1U);
    EXPECT_EQ(t.nodes[0].relation, 0);
    EXPECT_EQ(t.root, 0);
    EXPECT_EQ(CostOfTree(t), 0);
}

TEST(JoinOrder, TwoRelationsTheLargerProbesAndTheSmallerBuilds) {
    for (const bool flipped : {false, true}) {
        const std::vector<double> rows =
            flipped ? std::vector<double>{10, 1000} : std::vector<double>{1000, 10};
        const JoinTree t = ChooseJoinOrder(rows, {Equality(0, 1, 10, 10)});
        const JoinTree::Node& root = t.nodes[static_cast<size_t>(t.root)];
        EXPECT_EQ(t.nodes[static_cast<size_t>(root.left)].relation, flipped ? 1 : 0);
        EXPECT_EQ(t.nodes[static_cast<size_t>(root.right)].relation, flipped ? 0 : 1);
        EXPECT_NEAR(root.rows, 1000.0 * 10 / 10, 1e-9) << "a foreign key into a 10-value domain";
    }
}

TEST(JoinOrder, EqualSizedInputsAreTiedAndTheRelationWrittenFirstProbes) {
    // 100 and 100.00000000000001 rows are the same estimate: the first relation probes either way
    for (const double second : {100.0, 100.0 * (1 + 1e-13), 100.0 * (1 - 1e-13)}) {
        const JoinTree t = ChooseJoinOrder({100, second}, {Equality(0, 1, 100, 100)});
        const JoinTree::Node& root = t.nodes[static_cast<size_t>(t.root)];
        EXPECT_EQ(t.nodes[static_cast<size_t>(root.left)].relation, 0) << second;
        EXPECT_EQ(t.nodes[static_cast<size_t>(root.right)].relation, 1) << second;
    }
    // a real difference still decides
    const JoinTree t = ChooseJoinOrder({100, 101}, {Equality(0, 1, 100, 100)});
    EXPECT_EQ(t.nodes[static_cast<size_t>(t.nodes[static_cast<size_t>(t.root)].left)].relation, 1);
}

TEST(JoinOrder, EqualitiesBetweenTheSameTwoRelationsFormOneCompositeKey) {
    // two equalities with 10 distinct values each on 1000-row relations: 100 combinations
    EXPECT_NEAR(JoinSetRows({1000, 1000}, {Equality(0, 1, 10, 10), Equality(0, 1, 10, 10)}, 3),
                1000.0 * 1000 / 100, 1e-6);
    // 1000 distinct values each would claim 10^6 combinations: capped by the rows, as for a
    // (partkey, suppkey) pair against its own table
    EXPECT_NEAR(
        JoinSetRows({1000, 1000}, {Equality(0, 1, 1000, 1000), Equality(0, 1, 1000, 1000)}, 3),
        1000.0, 1e-6);
    // a single equality uses the larger number of distinct values
    EXPECT_NEAR(JoinSetRows({1000, 500}, {Equality(0, 1, 100, 400)}, 3), 1000.0 * 500 / 400, 1e-6);
    // a set that does not hold both relations does not feel the predicate
    EXPECT_NEAR(JoinSetRows({1000, 500, 7}, {Equality(0, 1, 100, 400)}, 5), 1000.0 * 7, 1e-6);
    // other predicates multiply in once all their relations are present
    EXPECT_NEAR(JoinSetRows({100, 100, 100}, {Other({0, 1, 2}, 0.1)}, 7), 100000.0, 1e-6);
    EXPECT_NEAR(JoinSetRows({100, 100, 100}, {Other({0, 1, 2}, 0.1)}, 3), 10000.0, 1e-6);
    // the combinations are capped by the larger relation's rows: at best one match per row
    EXPECT_EQ(JoinSetRows({10, 10}, {Equality(0, 1, 1e9, 1e9)}, 3), 10.0);
    // never below one row
    EXPECT_EQ(JoinSetRows({10, 10}, {Equality(0, 1, 1e9, 1e9), Other({0, 1}, 1e-9)}, 3), 1.0);
}

TEST(JoinOrder, ADynamicProgramFindsTheCheapestTreeOfAllOfThem) {
    Rng rng(101);
    for (int round = 0; round < 300; round++) {
        const size_t n = 2 + RandBelow(rng, 5); // 2..6 relations
        const std::vector<double> rows = RandomRows(rng, n);
        const std::vector<JoinEdge> edges = RandomEdges(rng, n, rows, /*connected=*/round % 4 != 0);
        const JoinTree tree = ChooseJoinOrder(rows, edges);
        const std::string what = "round " + std::to_string(round);
        CheckTree(tree, rows, edges, what);
        if (::testing::Test::HasFailure()) {
            return;
        }
        const double best = Brute{rows, edges}.Cost((uint64_t{1} << n) - 1);
        EXPECT_NEAR(CostOfTree(tree), best, 1e-9 * best) << what;
    }
}

TEST(JoinOrder, ConnectedGraphsAreNeverJoinedByACrossProduct) {
    Rng rng(102);
    for (int round = 0; round < 200; round++) {
        const size_t n = 2 + RandBelow(rng, 9); // up to 10
        // tiny relations make a cross product look cheap: it must still not be chosen
        std::vector<double> rows = RandomRows(rng, n);
        const std::vector<JoinEdge> edges = RandomEdges(rng, n, rows, true);
        const JoinTree tree = ChooseJoinOrder(rows, edges);
        EXPECT_FALSE(HasCrossProduct(tree, edges)) << "round " << round;
    }
}

TEST(JoinOrder, ADisconnectedGraphIsStillJoinedCompletely) {
    // {0, 1} and {2, 3} are not related: some cross product is unavoidable
    const std::vector<double> rows = {500, 20, 300, 7};
    const std::vector<JoinEdge> edges = {Equality(0, 1, 20, 20), Equality(2, 3, 7, 7)};
    const JoinTree tree = ChooseJoinOrder(rows, edges);
    CheckTree(tree, rows, edges, "disconnected");
    int cross = 0;
    for (const JoinTree::Node& n : tree.nodes) {
        if (n.relation >= 0) {
            continue;
        }
        const uint64_t a = tree.nodes[static_cast<size_t>(n.left)].set;
        const uint64_t b = tree.nodes[static_cast<size_t>(n.right)].set;
        const bool linked = std::any_of(edges.begin(), edges.end(), [&](const JoinEdge& e) {
            return (e.relations & ~n.set) == 0 && (e.relations & a) && (e.relations & b);
        });
        cross += linked ? 0 : 1;
    }
    EXPECT_EQ(cross, 1);
    // no predicate at all: a chain of cross products, the smallest first
    const JoinTree products = ChooseJoinOrder({100, 3, 50}, {});
    CheckTree(products, {100, 3, 50}, {}, "no edges");
}

TEST(JoinOrder, AStarJoinAppliesTheMostSelectiveDimensionFirstAndKeepsTheFactAsTheProbe) {
    // a fact table of a million rows and four dimensions whose join keys have 100 values each, but
    // the dimensions have been filtered down to 5 / 20 / 50 / 80 of them
    const std::vector<double> rows = {1e6, 80, 5, 50, 20};
    std::vector<JoinEdge> edges;
    for (int d = 1; d <= 4; d++) {
        edges.push_back(Equality(0, d, 100, 100));
    }
    const JoinTree tree = ChooseJoinOrder(rows, edges);
    CheckTree(tree, rows, edges, "star");
    // left-deep with the fact table at the bottom: every right child is a dimension
    int node = tree.root;
    std::vector<int> order; // dimensions in the order they are joined, last first
    while (tree.nodes[static_cast<size_t>(node)].relation < 0) {
        const JoinTree::Node& n = tree.nodes[static_cast<size_t>(node)];
        const JoinTree::Node& r = tree.nodes[static_cast<size_t>(n.right)];
        ASSERT_GE(r.relation, 1) << "a dimension builds";
        order.push_back(r.relation);
        node = n.left;
    }
    EXPECT_EQ(tree.nodes[static_cast<size_t>(node)].relation, 0) << "the fact table probes";
    std::reverse(order.begin(), order.end());
    EXPECT_EQ(order, (std::vector<int>{2, 4, 3, 1})) << "5, 20, 50 then 80 of the key values";
}

TEST(JoinOrder, ABushyTreeBeatsEveryLeftDeepOneWhenTwoPairsAreSelective) {
    // two small, selective pairs of large relations: joining each pair first and then the two
    // small results is far cheaper than any chain
    const std::vector<double> rows = {1e6, 1e6, 1e6, 1e6};
    const std::vector<JoinEdge> edges = {Equality(0, 1, 1e6, 1e6), Equality(2, 3, 1e6, 1e6),
                                         Other({0, 1, 2, 3}, 1e-6)};
    const JoinTree tree = ChooseJoinOrder(rows, edges);
    const JoinTree::Node& root = tree.nodes[static_cast<size_t>(tree.root)];
    const uint64_t a = tree.nodes[static_cast<size_t>(root.left)].set;
    const uint64_t b = tree.nodes[static_cast<size_t>(root.right)].set;
    EXPECT_TRUE((a == 3 && b == 12) || (a == 12 && b == 3)) << a << " / " << b;
}

TEST(JoinOrder, TheGreedyFallbackIsValidAndNeverBeatsTheExhaustiveSearch) {
    Rng rng(103);
    for (int round = 0; round < 150; round++) {
        const size_t n = 3 + RandBelow(rng, 7);
        const std::vector<double> rows = RandomRows(rng, n);
        const std::vector<JoinEdge> edges = RandomEdges(rng, n, rows, true);
        const JoinTree greedy = ChooseJoinOrderGreedy(rows, edges);
        const JoinTree best = ChooseJoinOrder(rows, edges);
        CheckTree(greedy, rows, edges, "greedy round " + std::to_string(round));
        EXPECT_FALSE(HasCrossProduct(greedy, edges));
        EXPECT_GE(CostOfTree(greedy), CostOfTree(best) * (1 - 1e-9)) << "round " << round;
    }
}

TEST(JoinOrder, MoreRelationsThanTheExhaustiveLimitUseTheGreedyOrder) {
    Rng rng(104);
    for (const size_t n : {kMaxExhaustiveRelations + 1, size_t{20}, kMaxJoinRelations}) {
        std::vector<double> rows = RandomRows(rng, n);
        const std::vector<JoinEdge> edges = RandomEdges(rng, n, rows, true);
        const JoinTree tree = ChooseJoinOrder(rows, edges);
        CheckTree(tree, rows, edges, "n = " + std::to_string(n));
        EXPECT_FALSE(HasCrossProduct(tree, edges)) << n;
        // the greedy order, exactly
        EXPECT_EQ(CostOfTree(tree), CostOfTree(ChooseJoinOrderGreedy(rows, edges)));
    }
}

TEST(JoinOrder, TheChoiceDoesNotDependOnHowTheRelationsAreNumbered) {
    Rng rng(105);
    for (int round = 0; round < 60; round++) {
        const size_t n = 3 + RandBelow(rng, 4);
        const std::vector<double> rows = RandomRows(rng, n);
        const std::vector<JoinEdge> edges = RandomEdges(rng, n, rows, true);
        // renumber the relations by a random permutation
        std::vector<int> perm(n);
        for (size_t i = 0; i < n; i++) {
            perm[i] = static_cast<int>(i);
        }
        for (size_t i = n - 1; i > 0; i--) {
            std::swap(perm[i], perm[RandBelow(rng, i + 1)]);
        }
        std::vector<double> rows2(n);
        for (size_t i = 0; i < n; i++) {
            rows2[static_cast<size_t>(perm[i])] = rows[i];
        }
        std::vector<JoinEdge> edges2 = edges;
        for (JoinEdge& e : edges2) {
            uint64_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if ((e.relations >> i) & 1) {
                    m |= uint64_t{1} << perm[i];
                }
            }
            e.relations = m;
            if (e.equality) {
                e.a = perm[static_cast<size_t>(e.a)];
                e.b = perm[static_cast<size_t>(e.b)];
            }
        }
        const double c1 = CostOfTree(ChooseJoinOrder(rows, edges));
        const double c2 = CostOfTree(ChooseJoinOrder(rows2, edges2));
        EXPECT_NEAR(c1, c2, 1e-9 * c1) << "round " << round;
    }
}

TEST(JoinOrder, TheSameInputGivesTheSameTree) {
    Rng rng(106);
    const std::vector<double> rows = RandomRows(rng, 7);
    const std::vector<JoinEdge> edges = RandomEdges(rng, 7, rows, true);
    const JoinTree a = ChooseJoinOrder(rows, edges), b = ChooseJoinOrder(rows, edges);
    ASSERT_EQ(a.nodes.size(), b.nodes.size());
    for (size_t i = 0; i < a.nodes.size(); i++) {
        EXPECT_EQ(a.nodes[i].set, b.nodes[i].set);
        EXPECT_EQ(a.nodes[i].left, b.nodes[i].left);
        EXPECT_EQ(a.nodes[i].right, b.nodes[i].right);
    }
}

} // namespace cdb
