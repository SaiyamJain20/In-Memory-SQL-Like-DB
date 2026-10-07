// The crash-injection campaign (ADR 0009, Phase 7 exit criterion): run a workload against a
// database whose file system dies at EVERY mutating operation in turn - every write, fsync, rename,
// remove and directory fsync - cut the power under several policies (nothing unsynced survives,
// everything does, random parts of it do, with torn and reordered writes), reopen, and check the
// one property that matters:
//
//   * every statement that returned success is there (durability), and
//   * the statement that was in flight is there completely or not at all, never partly (atomicity),
//   * nothing else is there.
//
// Then the same again with a second power cut in the middle of recovery itself.

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <thread>

namespace cdb {

namespace {

constexpr const char* kDir = "/db";

std::string Dump(Database& db) {
    std::string out;
    Connection conn(db);
    for (const std::string& name : db.catalog().ListTables()) {
        const std::shared_ptr<Table> t = db.catalog().GetTable(name);
        out += "TABLE " + t->name() + " (";
        for (const ColumnDefinition& c : t->schema()) {
            out += c.name + " " + c.type.ToString() + (c.not_null ? " NOT NULL" : "") + ", ";
        }
        out += ")\n";
        const QueryResult r = conn.Query("SELECT * FROM \"" + name + "\"");
        out += r.ok() ? r.ToString() : "ERROR " + r.error_message();
        out += "\n";
    }
    return out;
}

struct Workload {
    std::string name;
    std::vector<std::string> statements;
    DatabaseOptions options;
};

DatabaseOptions BaseOptions() {
    DatabaseOptions o;
    o.threads = 1;
    o.row_group_size = 2 * kVectorSize;
    return o;
}

// The state after 0, 1, 2, ... statements, computed on an in-memory database.
std::vector<std::string> ReferenceStates(const Workload& w) {
    Database db(1);
    Connection conn(db);
    std::vector<std::string> states = {Dump(db)};
    for (const std::string& sql : w.statements) {
        const QueryResult r = conn.Query(sql);
        EXPECT_TRUE(r.ok()) << sql << ": " << r.error_message();
        states.push_back(Dump(db));
    }
    return states;
}

struct RunResult {
    size_t acked = 0;       // statements that returned success
    bool in_flight = false; // a statement started and failed (the crash hit it)
    bool crashed_in_open = false;
    bool crashed = false;
};

// Runs the workload against `fi` (which may be set to crash). Everything the process would still be
// doing when it dies - the destructor included - fails quietly, as it would for a dead process.
RunResult Execute(const Workload& w, std::shared_ptr<FaultInjector> fi) {
    RunResult result;
    DatabaseOptions o = w.options;
    o.storage.fs = fi;
    std::unique_ptr<Database> db;
    try {
        db = std::make_unique<Database>(kDir, o);
    } catch (const Error&) {
        result.crashed_in_open = true;
        result.crashed = fi->crashed();
        return result;
    }
    {
        Connection conn(*db);
        for (const std::string& sql : w.statements) {
            const QueryResult r = conn.Query(sql);
            if (!r.ok()) {
                result.in_flight = true;
                break;
            }
            result.acked++;
        }
    }
    db.reset(); // closing: may checkpoint, may be cut short
    result.crashed = fi->crashed();
    return result;
}

std::vector<CrashPolicy> Policies(uint64_t n) {
    return {CrashPolicy::DropUnsynced(),    CrashPolicy::KeepAll(),
            CrashPolicy::Random(n * 7 + 1), CrashPolicy::Random(n * 7 + 2, true),
            CrashPolicy::Random(n * 7 + 3), CrashPolicy::Random(n * 7 + 4, true)};
}

// Opens the database in `fs` and returns its dump, or the error text.
std::string Recover(std::shared_ptr<FileSystem> fs, const DatabaseOptions& base,
                    std::string* error = nullptr) {
    DatabaseOptions o = base;
    o.storage.fs = std::move(fs);
    o.storage.checkpoint_on_close = false; // looking must not change what is on disk
    try {
        Database db(kDir, o);
        return Dump(db);
    } catch (const Error& e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return std::string("OPEN FAILED: ") + e.what();
    }
}

struct Stats {
    uint64_t crash_points = 0, recoveries = 0, nested = 0;
    uint64_t without_the_in_flight_statement = 0, with_the_in_flight_statement = 0;
};

// Sanitizer builds run every k-th crash point of the big workloads (the full set runs in the plain
// builds of the gate): they are 5-15x slower and add no coverage of the file format there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr uint64_t kBigStride = 3;
#else
constexpr uint64_t kBigStride = 1;
#endif

// The campaign for one workload. `nested_every`: also crash during recovery for every k-th crash
// point (0: never).
Stats Campaign(const Workload& w, bool full_sync, size_t nested_every, uint64_t stride = 1) {
    Stats stats;
    const std::vector<std::string> states = ReferenceStates(w);
    const std::set<std::string> all_states(states.begin(), states.end());

    // a clean run tells how many operations there are
    auto clean_mem = std::make_shared<MemoryFileSystem>();
    auto clean = std::make_shared<FaultInjector>(clean_mem);
    const RunResult baseline = Execute(w, clean);
    EXPECT_EQ(baseline.acked, w.statements.size()) << w.name;
    EXPECT_FALSE(baseline.crashed);
    const uint64_t total_ops = clean->operations();
    EXPECT_EQ(Recover(clean_mem, w.options), states.back())
        << w.name << ": a clean run must recover";

    for (uint64_t n = 0; n < total_ops; n += stride) {
        auto mem = std::make_shared<MemoryFileSystem>();
        auto fi = std::make_shared<FaultInjector>(mem);
        fi->CrashAtOperation(n);
        const RunResult run = Execute(w, fi);
        EXPECT_TRUE(run.crashed) << w.name << ": operation " << n << " was never reached";
        stats.crash_points++;

        // what the recovered database may legitimately be
        std::set<std::string> allowed;
        if (run.crashed_in_open) {
            allowed = {states[0]};
        } else if (run.in_flight) {
            allowed = {states[run.acked], states[run.acked + 1]};
        } else {
            allowed = {states[run.acked]}; // everything acknowledged; the crash hit the closing
        }
        if (!full_sync) { // without fsync only "some prefix" can be promised
            allowed.clear();
            for (size_t k = 0; k <= std::min(run.acked + 1, states.size() - 1); k++) {
                allowed.insert(states[k]);
            }
        }

        const std::vector<CrashPolicy> policies = Policies(n);
        for (size_t p = 0; p < policies.size(); p++) {
            const auto survivors = mem->Crash(policies[p]);
            std::string error;
            const std::string got = Recover(survivors, w.options, &error);
            stats.recoveries++;
            if (run.in_flight && got == states[run.acked + 1] &&
                states[run.acked + 1] != states[run.acked]) {
                stats.with_the_in_flight_statement++;
            } else if (run.in_flight && got == states[run.acked] &&
                       states[run.acked + 1] != states[run.acked]) {
                stats.without_the_in_flight_statement++;
            }
            if (allowed.count(got) == 0) {
                ADD_FAILURE() << w.name << ": crash at operation " << n << " (acknowledged "
                              << run.acked << " statements"
                              << (run.in_flight ? ", one in flight" : "")
                              << (run.crashed_in_open ? ", crashed opening" : "") << "), policy #"
                              << p << " recovered something that is not allowed.\n"
                              << (error.empty() ? "" : "error: " + error + "\n") << "got:\n"
                              << got.substr(0, 1500) << "\nallowed (" << allowed.size()
                              << "), first:\n"
                              << allowed.begin()->substr(0, 1500);
                return stats; // one failure is enough; the rest would drown it
            }

            // a second power cut, in the middle of recovering from the first
            if (nested_every != 0 && n % nested_every == 0 && (p == 0 || p == 3)) {
                auto probe_mem =
                    survivors->Crash(CrashPolicy::KeepAll()); // a copy to count operations on
                auto probe = std::make_shared<FaultInjector>(probe_mem);
                DatabaseOptions o = w.options;
                o.storage.fs = probe;
                o.storage.checkpoint_on_close = false;
                try {
                    Database db(kDir, o);
                } catch (const Error&) {
                }
                const uint64_t recovery_ops = probe->operations();
                for (uint64_t m = 0; m < recovery_ops; m++) {
                    auto copy = survivors->Crash(CrashPolicy::KeepAll());
                    auto inj = std::make_shared<FaultInjector>(copy);
                    inj->CrashAtOperation(m);
                    DatabaseOptions o2 = w.options;
                    o2.storage.fs = inj;
                    o2.storage.checkpoint_on_close = false;
                    try {
                        Database db(kDir, o2);
                    } catch (const Error&) {
                    }
                    for (const CrashPolicy& second :
                         {CrashPolicy::DropUnsynced(), CrashPolicy::Random(m + n * 31, true)}) {
                        const auto again = copy->Crash(second);
                        const std::string got2 = Recover(again, w.options);
                        stats.nested++;
                        if (got2 != got) {
                            ADD_FAILURE() << w.name << ": a power cut at operation " << m
                                          << " of the recovery after the crash at operation " << n
                                          << " (policy #" << p
                                          << ") changed what the database holds.\nbefore:\n"
                                          << got.substr(0, 800) << "\nafter:\n"
                                          << got2.substr(0, 800);
                            return stats;
                        }
                    }
                }
            }
        }
    }
    return stats;
}

// A disk that errors instead of dying: operation n fails once (a full disk, a bad sector, a failed
// fsync) and the process lives on. The statement that hit it fails, the live database must not have
// applied it, and it must refuse to carry on in a state it cannot vouch for; after the process then
// dies for any reason, what comes back is still an acknowledged prefix plus possibly that
// statement.
Stats ErrorCampaign(const Workload& w) {
    Stats stats;
    const std::vector<std::string> states = ReferenceStates(w);
    auto clean_mem = std::make_shared<MemoryFileSystem>();
    auto clean = std::make_shared<FaultInjector>(clean_mem);
    EXPECT_EQ(Execute(w, clean).acked, w.statements.size());
    const uint64_t total_ops = clean->operations();

    for (uint64_t n = 0; n < total_ops; n++) {
        auto mem = std::make_shared<MemoryFileSystem>();
        auto fi = std::make_shared<FaultInjector>(mem);
        fi->FailOperation(n);
        DatabaseOptions o = w.options;
        o.storage.fs = fi;
        size_t acked = 0;
        bool failed_statement = false;
        std::string live_state;
        {
            std::unique_ptr<Database> db;
            try {
                db = std::make_unique<Database>(kDir, o);
            } catch (const Error&) {
                // the error hit while opening: nothing was acknowledged
            }
            if (db != nullptr) {
                Connection conn(*db);
                for (const std::string& sql : w.statements) {
                    const QueryResult r = conn.Query(sql);
                    if (!r.ok()) {
                        failed_statement = true;
                        break;
                    }
                    acked++;
                }
                if (failed_statement) {
                    live_state = Dump(*db);
                    // the failed statement changed nothing in memory
                    if (live_state != states[acked]) {
                        ADD_FAILURE() << w.name << ": after an I/O error at operation " << n
                                      << " the live database differs from its acknowledged state";
                        return stats;
                    }
                    stats.crash_points++;
                    // and unless the error was in the closing, it refuses further changes... or, if
                    // the log is intact (a failed snapshot), keeps working; either way never
                    // corrupts
                }
            }
            // `db` closes here; a failed log may refuse to checkpoint, a healthy one checkpoints
        }
        std::set<std::string> allowed = {states[0]};
        for (size_t k = acked; k <= std::min(acked + (failed_statement ? 1 : 0), states.size() - 1);
             k++) {
            allowed.insert(states[k]);
        }
        allowed.erase(states[0]);
        if (acked == 0) {
            allowed.insert(states[0]);
        }
        if (!failed_statement && acked == w.statements.size()) {
            allowed = {states.back()};
        }
        for (const CrashPolicy& policy : {CrashPolicy::DropUnsynced(), CrashPolicy::KeepAll(),
                                          CrashPolicy::Random(n * 5 + 1, true)}) {
            const auto survivors = mem->Crash(policy);
            const std::string got = Recover(survivors, w.options);
            stats.recoveries++;
            if (allowed.count(got) == 0) {
                ADD_FAILURE() << w.name << ": an I/O error at operation " << n << " (acknowledged "
                              << acked << (failed_statement ? ", one failed" : "")
                              << ") left a database that recovers to a state that is not allowed:\n"
                              << got.substr(0, 1200);
                return stats;
            }
        }
    }
    return stats;
}

Workload Basic() {
    Workload w;
    w.name = "basic";
    w.options = BaseOptions();
    w.options.storage.checkpoint_wal_bytes = 150; // automatic checkpoints, often
    w.statements = {
        "CREATE TABLE a (id BIGINT NOT NULL, name VARCHAR, price DOUBLE, d DATE, flag BOOLEAN)",
        "INSERT INTO a VALUES (1, 'x', 1.5, DATE '2020-01-02', true), (2, NULL, NULL, NULL, NULL)",
        "CREATE TABLE b (k INTEGER, v VARCHAR)",
        "INSERT INTO b VALUES (1, 'one'), (2, 'a string that is longer than twelve bytes'), (3, "
        "NULL)",
        "INSERT INTO a SELECT id + 10, name, price * 2, d, flag FROM a",
        "CHECKPOINT",
        "DROP TABLE b",
        "CREATE TABLE b (k BIGINT, tag VARCHAR)",
        "INSERT INTO b SELECT id, name FROM a",
        "CREATE TABLE IF NOT EXISTS a (z INTEGER)",
        "INSERT INTO b SELECT k + 100, tag FROM b",
        "INSERT INTO a SELECT id + 100, name, price, d, flag FROM a",
        "DROP TABLE a",
        "INSERT INTO b VALUES (7, 'last')",
        // a third table: one more shape of segment (DATE and DOUBLE with NULLs, a constant
        // column, a low-cardinality one), written by the checkpoints that the log size triggers
        "CREATE TABLE c (d DATE, x DOUBLE, tag VARCHAR, n INTEGER)",
        "INSERT INTO c VALUES (DATE '2021-05-06', 0.5, 'p', 1), (NULL, NULL, 'p', 1), (DATE "
        "'1999-12-31', -2.25, 'q', 1)",
        "INSERT INTO c SELECT d, x * 2, tag, n FROM c",
        "INSERT INTO b SELECT k + 1000, tag FROM b WHERE k < 200",
        "DROP TABLE c",
    };
    return w;
}

Workload NoAutomaticCheckpoints() {
    Workload w = Basic();
    w.name = "log only, explicit checkpoints";
    w.options.storage.checkpoint_wal_bytes = 0;
    return w;
}

// Several row groups and a statement that spans several frames: atomicity of one big statement.
Workload Bulk() {
    Workload w;
    w.name = "bulk";
    w.options = BaseOptions();
    w.options.storage.checkpoint_wal_bytes = 0;
    w.options.storage.max_frame_bytes = 1; // every chunk of a statement is its own frame
    w.statements = {
        "CREATE TABLE t (id BIGINT, s VARCHAR)",
        "INSERT INTO t VALUES (1, 'seed'), (2, 'two')",
    };
    for (int i = 0; i < 11; i++) { // doubling: 4, 8, ... 4096 rows
        w.statements.push_back("INSERT INTO t SELECT id + " + std::to_string(1 << (i + 1)) +
                               ", s FROM t");
    }
    w.statements.push_back("CHECKPOINT");
    w.statements.push_back("INSERT INTO t VALUES (-1, 'after the checkpoint')");
    return w;
}

} // namespace

TEST(CrashCampaign, EveryOperationOfAMixedWorkloadWithAutomaticCheckpoints) {
    const Stats s = Campaign(Basic(), true, 4);
    EXPECT_GT(s.crash_points, 140U);
    EXPECT_GT(s.recoveries, 800U);
    EXPECT_GT(s.nested, 300U);
    // the campaign must really hit both outcomes of the in-flight statement: lost, and kept whole
    EXPECT_GT(s.without_the_in_flight_statement, 20U);
    EXPECT_GT(s.with_the_in_flight_statement, 20U);
    std::cout << "[campaign] basic: " << s.crash_points << " crash points, " << s.recoveries
              << " recoveries, " << s.nested << " nested recoveries\n";
}

TEST(CrashCampaign, EveryOperationWithTheLogAloneAndExplicitCheckpoints) {
    const Stats s = Campaign(NoAutomaticCheckpoints(), true, 5);
    EXPECT_GT(s.crash_points, 60U);
    EXPECT_GT(s.without_the_in_flight_statement, 10U);
    EXPECT_GT(s.with_the_in_flight_statement, 10U);
    std::cout << "[campaign] log only: " << s.crash_points << " crash points, " << s.recoveries
              << " recoveries, " << s.nested << " nested recoveries\n";
}

TEST(CrashCampaign, ABigMultiFrameStatementIsAllOrNothingWhereverThePowerFails) {
    const Stats s = Campaign(Bulk(), true, 9, kBigStride);
    // (the operations covered: a sanitizer build runs every kBigStride-th crash point of them)
    EXPECT_GT(s.crash_points * kBigStride, 70U);
    EXPECT_GT(s.without_the_in_flight_statement, 5U);
    EXPECT_GT(s.with_the_in_flight_statement, 5U);
    std::cout << "[campaign] bulk: " << s.crash_points << " crash points, " << s.recoveries
              << " recoveries, " << s.nested << " nested recoveries\n";
}

TEST(CrashCampaign, AnIoErrorAtAnyOperationFailsCleanlyAndNeverLeavesAnythingElseBehind) {
    for (const Workload& w : {Basic(), NoAutomaticCheckpoints(), Bulk()}) {
        const Stats s = ErrorCampaign(w);
        std::cout << "[campaign] I/O errors, " << w.name << ": " << s.crash_points
                  << " statements failed by an injected error, " << s.recoveries << " recoveries\n";
        EXPECT_GT(s.crash_points, 30U)
            << w.name << ": the errors should mostly land inside statements";
        EXPECT_GT(s.recoveries, 200U);
    }
}

// Several sessions commit at once while checkpoints rotate the log underneath them, and the power
// fails at a random operation. Each session owns a table and inserts 0, 1, 2, ... into it, so what
// recovery must find is easy to state: for every table, the rows 0..m-1 with m between the number
// of acknowledged inserts and one more. (This is the test that sees a log whose *name* is not yet
// durable while commits are already being acknowledged into it.)
TEST(CrashCampaign, ConcurrentSessionsAndCheckpointsSurviveAPowerCutAnywhere) {
    constexpr int kThreads = 4;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr uint64_t kRounds = 50;
#else
    constexpr uint64_t kRounds = 160;
#endif
    uint64_t crashed_rounds = 0, checkpoints_seen = 0;
    for (uint64_t seed = 0; seed < kRounds; seed++) {
        std::mt19937_64 rng(seed * 7919 + 1);
        auto mem = std::make_shared<MemoryFileSystem>();
        auto fi = std::make_shared<FaultInjector>(mem);
        DatabaseOptions o = BaseOptions();
        o.storage.fs = fi;
        o.storage.checkpoint_wal_bytes = 300 + rng() % 600;
        fi->CrashAtOperation(12 + rng() % 600);
        std::vector<int64_t> acked(kThreads, 0);
        std::vector<char> created(kThreads, 0);
        {
            std::unique_ptr<Database> db;
            try {
                db = std::make_unique<Database>(kDir, o);
            } catch (const Error&) {
            }
            if (db != nullptr) {
                std::vector<std::thread> threads;
                for (int t = 0; t < kThreads; t++) {
                    threads.emplace_back([&, t] {
                        Connection conn(*db);
                        const std::string table = "own" + std::to_string(t);
                        if (!conn.Query("CREATE TABLE " + table + " (i BIGINT, pad VARCHAR)")
                                 .ok()) {
                            return;
                        }
                        created[static_cast<size_t>(t)] = 1;
                        for (int64_t i = 0; i < 120; i++) {
                            if (!conn.Query("INSERT INTO " + table + " VALUES (" +
                                            std::to_string(i) +
                                            ", 'some padding to make the log grow')")
                                     .ok()) {
                                return;
                            }
                            acked[static_cast<size_t>(t)] = i + 1;
                        }
                    });
                }
                for (auto& th : threads) {
                    th.join();
                }
                checkpoints_seen += db->storage()->checkpoint_epoch() > 0 ? 1 : 0;
            }
        }
        crashed_rounds += fi->crashed() ? 1 : 0;
        for (const CrashPolicy& policy :
             {CrashPolicy::DropUnsynced(), CrashPolicy::KeepAll(), CrashPolicy::Random(seed, false),
              CrashPolicy::Random(seed, true)}) {
            DatabaseOptions ro = BaseOptions();
            ro.storage.fs = mem->Crash(policy);
            ro.storage.checkpoint_on_close = false;
            std::unique_ptr<Database> db;
            try {
                db = std::make_unique<Database>(kDir, ro);
            } catch (const Error& e) {
                ADD_FAILURE() << "seed " << seed << ": the database does not reopen: " << e.what();
                return;
            }
            Connection conn(*db);
            for (int t = 0; t < kThreads; t++) {
                const std::string table = "own" + std::to_string(t);
                if (db->catalog().TryGetTable(table) == nullptr) {
                    EXPECT_FALSE(created[static_cast<size_t>(t)])
                        << "seed " << seed << ": the acknowledged table " << table << " is gone";
                    continue;
                }
                const QueryResult r = conn.Query("SELECT i FROM " + table);
                ASSERT_TRUE(r.ok());
                const auto rows = static_cast<int64_t>(r.RowCount());
                if (rows < acked[static_cast<size_t>(t)] ||
                    rows > acked[static_cast<size_t>(t)] + 1) {
                    ADD_FAILURE() << "seed " << seed << ": " << table << " holds " << rows
                                  << " rows after " << acked[static_cast<size_t>(t)]
                                  << " acknowledged inserts (the power failed at operation count "
                                  << fi->operations() << ")";
                    return;
                }
                for (int64_t i = 0; i < rows; i++) {
                    ASSERT_EQ(r.GetValue(0, static_cast<idx_t>(i)), Value::BigInt(i))
                        << "seed " << seed << " " << table << ": rows out of order or missing";
                }
            }
        }
    }
    EXPECT_GT(crashed_rounds, kRounds / 2) << "most rounds should lose power mid-workload";
    EXPECT_GT(checkpoints_seen, kRounds / 4) << "checkpoints should be taken while sessions commit";
}

TEST(CrashCampaign, WithoutFsyncTheRecoveredStateIsStillACommittedPrefix) {
    Workload w = Basic();
    w.name = "basic, SyncMode::Off";
    w.options.storage.sync = SyncMode::Off;
    const Stats s = Campaign(w, false, 0);
    EXPECT_GT(s.recoveries, 800U);
}

} // namespace cdb
