// The physical planner's choices - which operators, which pipelines, which join implementation and
// which side builds - cannot be seen in query results, so they are checked on the plan itself.

#include "execution/physical_planner.h"

#include "execution/basic_operators.h"
#include "main/connection.h"
#include "planner/optimizer.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

struct Env {
    Database db;
    Connection conn{db};

    Env() {
        Run("CREATE TABLE a (x INTEGER, y VARCHAR)");
        Run("CREATE TABLE b (x INTEGER, z DOUBLE)");
        Run("INSERT INTO a VALUES (1, 'p'), (2, 'q'), (3, 'r')");
        Run("INSERT INTO b VALUES (2, 0.5), (3, 1.5), (4, 2.5), (3, 3.5)");
    }
    void Run(const std::string& sql) {
        const QueryResult r = conn.Query(sql);
        ASSERT_TRUE(r.ok()) << r.error_message();
    }
    std::unique_ptr<PhysicalPlan> Plan(const std::string& sql) {
        return PlanSelect(*Optimize(conn.Plan(sql)));
    }
    std::string Text(const std::string& sql) { return Plan(sql)->ToString(); }
};

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST(PhysicalPlanner, OrderByWithLimitIsATopNAndWithoutOneIsASort) {
    Env env;
    const std::string top = env.Text("SELECT x FROM a ORDER BY x LIMIT 2");
    EXPECT_TRUE(Has(top, "TOP_N")) << top;
    EXPECT_TRUE(Has(top, "LIMIT 2"));
    EXPECT_FALSE(Has(top, "ORDER_BY")) << top;
    const std::string with_offset = env.Text("SELECT x FROM a ORDER BY x LIMIT 2 OFFSET 1");
    EXPECT_TRUE(Has(with_offset, "TOP_N") && Has(with_offset, "OFFSET 1")) << with_offset;
    const std::string sort = env.Text("SELECT x FROM a ORDER BY x");
    EXPECT_TRUE(Has(sort, "ORDER_BY")) << sort;
    EXPECT_FALSE(Has(sort, "TOP_N")) << sort;
    const std::string offset_only = env.Text("SELECT x FROM a ORDER BY x OFFSET 1");
    EXPECT_TRUE(Has(offset_only, "ORDER_BY") && Has(offset_only, "LIMIT ALL OFFSET 1"))
        << offset_only;
    const std::string plain_limit = env.Text("SELECT x FROM a LIMIT 2");
    EXPECT_TRUE(Has(plain_limit, "LIMIT 2") && !Has(plain_limit, "TOP_N") &&
                !Has(plain_limit, "ORDER_BY"));
}

TEST(PhysicalPlanner, EqualityConjunctsBecomeHashKeysAndTheRestAResidual) {
    Env env;
    const std::string eq = env.Text("SELECT a.y FROM a JOIN b ON a.x = b.x");
    EXPECT_TRUE(Has(eq, "HASH_JOIN INNER ON")) << eq;
    EXPECT_FALSE(Has(eq, "WHERE")) << "no residual: " << eq;

    const std::string mixed = env.Text("SELECT a.y FROM a JOIN b ON a.x = b.x AND b.z > a.x");
    EXPECT_TRUE(Has(mixed, "HASH_JOIN") && Has(mixed, " WHERE ")) << mixed;

    const std::string range = env.Text("SELECT a.y FROM a JOIN b ON a.x < b.x");
    EXPECT_TRUE(Has(range, "NESTED_LOOP_JOIN") && Has(range, " WHERE ")) << range;
    EXPECT_FALSE(Has(range, "HASH_JOIN")) << range;

    const std::string cross = env.Text("SELECT a.y FROM a, b");
    EXPECT_TRUE(Has(cross, "NESTED_LOOP_JOIN INNER")) << cross;

    const std::string two_keys =
        env.Text("SELECT a.y FROM a JOIN b ON a.x = b.x AND a.y = CAST(b.z AS VARCHAR)");
    EXPECT_TRUE(Has(two_keys, " AND ")) << two_keys;
}

TEST(PhysicalPlanner, JoinBuildSideIsItsOwnPipelineThatRunsFirst) {
    Env env;
    const auto plan =
        env.Plan("SELECT a.y, count(*) FROM a JOIN b ON a.x = b.x GROUP BY a.y ORDER BY a.y");
    ASSERT_GE(plan->pipelines.size(), 4U) << plan->ToString();
    // every pipeline depends only on earlier ones
    for (size_t i = 0; i < plan->pipelines.size(); i++) {
        for (const size_t d : plan->pipelines[i].dependencies) {
            EXPECT_LT(d, i) << plan->ToString();
        }
    }
    // the join appears as a streaming operator in exactly one pipeline and as the sink of another
    int as_operator = 0, as_sink = 0;
    for (const Pipeline& p : plan->pipelines) {
        for (const PhysicalOperator* op : p.operators) {
            as_operator += op->Name() == "HASH_JOIN";
        }
        as_sink += p.sink->Name() == "HASH_JOIN";
    }
    EXPECT_EQ(as_operator, 1);
    EXPECT_EQ(as_sink, 1);
    EXPECT_EQ(plan->root->Name(), "RESULT");
}

TEST(PhysicalPlanner, RightJoinRunsAsALeftJoinWithTheSidesSwapped) {
    Env env;
    const auto plan = env.Plan("SELECT a.x, a.y, b.x, b.z FROM a RIGHT JOIN b ON a.x = b.x");
    const std::string text = plan->ToString();
    EXPECT_TRUE(Has(text, "HASH_JOIN LEFT")) << text;
    EXPECT_FALSE(Has(text, "RIGHT")) << text;
    // b (the preserved side) streams through the join; a is the build side
    const Pipeline* probe = nullptr;
    const Pipeline* build = nullptr;
    for (const Pipeline& p : plan->pipelines) {
        if (p.sink->Name() == "HASH_JOIN") {
            build = &p;
        } else if (!p.operators.empty()) {
            probe = &p;
        }
    }
    ASSERT_TRUE(build != nullptr && probe != nullptr) << text;
    EXPECT_TRUE(Has(build->source->Describe(), " a ")) << build->source->Describe();
    EXPECT_TRUE(Has(probe->source->Describe(), " b ")) << probe->source->Describe();
    // the output columns are restored to left ++ right order by a projection after the join
    EXPECT_EQ(probe->operators.back()->Name(), "PROJECTION") << text;
}

TEST(PhysicalPlanner, FullOuterJoinIsReportedNotImplemented) {
    Env env;
    EXPECT_THROW(env.Plan("SELECT a.x FROM a FULL JOIN b ON a.x = b.x"), Error);
    try {
        env.Plan("SELECT a.x FROM a FULL JOIN b ON a.x = b.x");
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::NotImplemented);
    }
}

TEST(PhysicalPlanner, DistinctGroupsByEveryColumn) {
    Env env;
    const std::string text = env.Text("SELECT DISTINCT y, x FROM a");
    EXPECT_TRUE(Has(text, "HASH_GROUP_BY")) << text;
}

TEST(PhysicalPlanner, GlobalAggregateHasNoGroupBy) {
    Env env;
    const std::string text = env.Text("SELECT count(*), sum(x) FROM a");
    EXPECT_TRUE(Has(text, "UNGROUPED_AGGREGATE")) << text;
    EXPECT_FALSE(Has(text, "HASH_GROUP_BY")) << text;
}

TEST(PhysicalPlanner, AllScansOfATableInOneQueryShareOneSnapshot) {
    Env env;
    // Plan a self-join, then change the table, then run the plan: the plan was built from the
    // snapshot taken at planning time, so both scans still see the three original rows.
    auto plan = env.Plan("SELECT count(*) FROM a AS p, a AS q");
    env.Run("INSERT INTO a VALUES (4, 's'), (5, 't')");
    Executor executor(*plan);
    executor.Run();
    const auto chunks = PhysicalResultCollector::TakeChunks(*executor.SinkState(*plan->root));
    ASSERT_EQ(chunks.size(), 1U);
    EXPECT_EQ(chunks[0].GetValue(0, 0), Value::BigInt(9)) << "3 x 3 rows, not 5 x 3 or 5 x 5";
    EXPECT_EQ(env.conn.Query("SELECT count(*) FROM a AS p, a AS q").GetValue(0, 0),
              Value::BigInt(25));
}

} // namespace cdb
