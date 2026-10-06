// A persistent Database end to end: every statement reaches the disk, reopening gives back exactly
// the same tables, checkpoints and the log cooperate, damaged directories are refused with a clear
// error, and a failing disk makes the database read-only instead of wrong.

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"
#include "storage/binary_io.h"
#include "storage/checksum.h"
#include "storage/segment_io.h"
#include "storage/wal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include <unistd.h>

namespace cdb {

namespace {

constexpr const char* kDir = "/db";

std::shared_ptr<MemoryFileSystem> NewFs() {
    return std::make_shared<MemoryFileSystem>();
}

DatabaseOptions Options(std::shared_ptr<FileSystem> fs) {
    DatabaseOptions o;
    o.threads = 1;
    o.storage.fs = std::move(fs);
    o.row_group_size = 2 * kVectorSize; // small groups: many boundaries on small data
    return o;
}

std::unique_ptr<Database> Open(std::shared_ptr<FileSystem> fs, DatabaseOptions options) {
    options.storage.fs = std::move(fs);
    return std::make_unique<Database>(kDir, std::move(options));
}

std::unique_ptr<Database> Open(std::shared_ptr<FileSystem> fs) {
    return std::make_unique<Database>(kDir, Options(std::move(fs)));
}

void Exec(Connection& conn, const std::string& sql) {
    const QueryResult r = conn.Query(sql);
    ASSERT_TRUE(r.ok()) << sql << ": " << r.error_message();
}

// Every table and every row, in a form two databases can be compared by.
std::string Dump(Database& db) {
    std::string out;
    Connection conn(db);
    for (const std::string& name : db.catalog().ListTables()) {
        const std::shared_ptr<Table> t = db.catalog().GetTable(name);
        out += "TABLE " + t->name() + " (";
        for (const ColumnDefinition& c : t->schema()) {
            out += c.name + " " + c.type.ToString() + (c.not_null ? " NOT NULL" : "") + ", ";
        }
        out += ") rows=" + std::to_string(t->RowCount()) + "\n";
        const QueryResult r = conn.Query("SELECT * FROM \"" + name + "\"");
        out += r.ok() ? r.ToString() : "ERROR " + r.error_message();
        out += "\n";
    }
    return out;
}

const std::vector<std::string>& Script() {
    static const std::vector<std::string> kScript = {
        "CREATE TABLE a (id BIGINT NOT NULL, name VARCHAR, price DOUBLE, d DATE, flag BOOLEAN, n "
        "INTEGER)",
        "INSERT INTO a VALUES (1, 'x', 1.5, DATE '2020-01-02', true, 1), "
        "(2, NULL, NULL, NULL, NULL, NULL), (3, 'a string longer than twelve bytes', -0.0, "
        "DATE '1999-12-31', false, -7)",
        "CREATE TABLE \"Mixed Case\" (k INTEGER, v VARCHAR)",
        "INSERT INTO \"Mixed Case\" VALUES (1, 'one'), (2, 'two'), (3, NULL)",
        "INSERT INTO a SELECT id + 10, name, price * 2, d, flag, n FROM a",
        "CREATE TABLE IF NOT EXISTS a (z INTEGER)",
        "DROP TABLE \"Mixed Case\"",
        "CREATE TABLE b (k BIGINT)",
        "INSERT INTO b SELECT id FROM a",
        "INSERT INTO b SELECT k + 100 FROM b",
        "DROP TABLE IF EXISTS never_existed",
        "INSERT INTO a SELECT id + 100, name, price, d, flag, n FROM a",
    };
    return kScript;
}

void RunScript(Connection& conn, const std::vector<std::string>& script) {
    for (const std::string& sql : script) {
        Exec(conn, sql);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

std::string ReferenceDump(const std::vector<std::string>& script) {
    Database db(1);
    Connection conn(db);
    RunScript(conn, script);
    return Dump(db);
}

// Doubles a table with INSERT ... SELECT until it has at least `rows` rows.
void Grow(Connection& conn, const std::string& table, idx_t at_least) {
    for (;;) {
        const QueryResult r = conn.Query("SELECT count(*) FROM " + table);
        ASSERT_TRUE(r.ok());
        const auto n = static_cast<idx_t>(r.GetValue(0, 0).GetBigInt());
        if (n >= at_least) {
            return;
        }
        Exec(conn, "INSERT INTO " + table + " SELECT id + " + std::to_string(n) +
                       ", name, price, d, flag, n FROM " + table);
    }
}

std::vector<std::string> Files(MemoryFileSystem& fs) {
    std::vector<std::string> names;
    for (const std::string& n : fs.List(kDir)) {
        names.push_back(n);
    }
    return names;
}

bool Has(MemoryFileSystem& fs, const std::string& name) {
    return fs.Exists(std::string(kDir) + "/" + name);
}

void ExpectError(const std::function<void()>& fn, ErrorCode code,
                 const std::string& contains = "") {
    try {
        fn();
        ADD_FAILURE() << "expected an error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), code) << e.what();
        EXPECT_NE(std::string(e.what()).find(contains), std::string::npos) << e.what();
    }
}

} // namespace

TEST(Persistence, ADirectoryWithNoDatabaseStartsANewOne) {
    auto fs = NewFs();
    {
        auto db = Open(fs);
        EXPECT_TRUE(db->persistent());
        EXPECT_TRUE(db->storage()->recovery().created);
        EXPECT_FALSE(db->storage()->recovery().had_checkpoint);
        EXPECT_EQ(db->storage()->epoch(), 0U);
        EXPECT_TRUE(Has(*fs, "wal-0000000000000000.log"));
        EXPECT_TRUE(db->catalog().ListTables().empty());
    }
    // closing an unused database writes no checkpoint
    EXPECT_FALSE(Has(*fs, "checkpoint-0000000000000001.cdb"));
    auto again = Open(fs);
    EXPECT_FALSE(again->storage()->recovery().created) << "the log of the first life is there";
}

TEST(Persistence, AnInMemoryDatabaseHasNoStorage) {
    Database db(1);
    EXPECT_FALSE(db.persistent());
    EXPECT_EQ(db.storage(), nullptr);
    Connection conn(db);
    Exec(conn, "CREATE TABLE t (a INTEGER)");
    EXPECT_TRUE(conn.Query("CHECKPOINT").ok()) << "a no-op without storage";
}

TEST(Persistence, EveryStatementSurvivesReopeningFromTheLogAlone) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, Script());
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        EXPECT_EQ(Dump(*db), ReferenceDump(Script()));
    }
    EXPECT_FALSE(Has(*fs, "checkpoint-0000000000000001.cdb")) << "no checkpoint was asked for";
    auto db = Open(fs, o);
    const RecoveryStats& r = db->storage()->recovery();
    EXPECT_FALSE(r.had_checkpoint);
    EXPECT_EQ(r.wal_files, 1U);
    EXPECT_EQ(r.transactions, 10U)
        << "12 statements, of which IF NOT EXISTS and IF EXISTS changed nothing";
    EXPECT_EQ(r.discarded_bytes, 0U);
    EXPECT_EQ(Dump(*db), ReferenceDump(Script()));
}

TEST(Persistence, ClosingCheckpointsAndReopeningLoadsTheCheckpoint) {
    auto fs = NewFs();
    {
        auto db = Open(fs);
        Connection conn(*db);
        RunScript(conn, Script());
    }
    EXPECT_TRUE(Has(*fs, "checkpoint-0000000000000001.cdb"));
    EXPECT_TRUE(Has(*fs, "wal-0000000000000001.log"));
    EXPECT_FALSE(Has(*fs, "wal-0000000000000000.log"))
        << "the old log is folded into the checkpoint";
    auto db = Open(fs);
    const RecoveryStats& r = db->storage()->recovery();
    EXPECT_TRUE(r.had_checkpoint);
    EXPECT_EQ(r.checkpoint_epoch, 1U);
    EXPECT_EQ(r.tables, 2U);
    EXPECT_EQ(r.transactions, 0U);
    EXPECT_EQ(Dump(*db), ReferenceDump(Script()));
}

TEST(Persistence, TheCheckpointStatementFoldsTheLogAndLaterStatementsGoToTheNewOne) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_on_close = false;
    std::vector<std::string> first(Script().begin(), Script().begin() + 6);
    std::vector<std::string> second(Script().begin() + 6, Script().end());
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, first);
        Exec(conn, "CHECKPOINT");
        EXPECT_EQ(db->storage()->checkpoint_epoch(), 1U);
        EXPECT_EQ(db->storage()->epoch(), 1U);
        EXPECT_TRUE(Has(*fs, "checkpoint-0000000000000001.cdb"));
        EXPECT_FALSE(Has(*fs, "wal-0000000000000000.log"));
        RunScript(conn, second);
    }
    auto db = Open(fs, o);
    EXPECT_TRUE(db->storage()->recovery().had_checkpoint);
    EXPECT_GT(db->storage()->recovery().transactions, 0U)
        << "the later statements come from the new log";
    EXPECT_EQ(Dump(*db), ReferenceDump(Script()));
}

TEST(Persistence, ACheckpointWithNothingNewIsANoOp) {
    auto fs = NewFs();
    auto db = Open(fs);
    Connection conn(*db);
    Exec(conn, "CREATE TABLE t (a INTEGER)");
    Exec(conn, "CHECKPOINT");
    EXPECT_EQ(db->storage()->checkpoint_epoch(), 1U);
    const auto files = Files(*fs);
    Exec(conn, "CHECKPOINT");
    Exec(conn, "CHECKPOINT");
    EXPECT_EQ(db->storage()->checkpoint_epoch(), 1U);
    EXPECT_EQ(Files(*fs), files) << "no new files";
    Exec(conn, "INSERT INTO t VALUES (1)");
    Exec(conn, "CHECKPOINT");
    EXPECT_EQ(db->storage()->checkpoint_epoch(), 2U);
}

TEST(Persistence, TheLogIsFoldedAutomaticallyWhenItGrowsPastTheThreshold) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_wal_bytes = 2000;
    o.storage.checkpoint_on_close = false;
    std::vector<std::string> script = {"CREATE TABLE t (id BIGINT, s VARCHAR)"};
    for (int i = 0; i < 40; i++) {
        script.push_back("INSERT INTO t VALUES (" + std::to_string(i) + ", '" +
                         std::string(100, 'a' + i % 26) + "')");
    }
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, script);
        EXPECT_GE(db->storage()->checkpoint_epoch(), 2U)
            << "40 x ~145 bytes against a 2000-byte threshold: after the 14th and the 28th";
        EXPECT_LT(db->storage()->uncheckpointed_bytes(), 2000U + 400U);
        EXPECT_EQ(db->storage()->last_checkpoint_error(), "");
        // old files do not pile up: one checkpoint and the logs since
        size_t checkpoints = 0, logs = 0;
        for (const std::string& n : Files(*fs)) {
            checkpoints += n.rfind("checkpoint-", 0) == 0;
            logs += n.rfind("wal-", 0) == 0;
        }
        EXPECT_EQ(checkpoints, 1U);
        EXPECT_EQ(logs, 1U);
    }
    auto db = Open(fs, o);
    EXPECT_EQ(Dump(*db), ReferenceDump(script));
}

TEST(Persistence, TablesWithManyRowGroupsAndAPartialTailRoundTripThroughACheckpointAndTheLog) {
    for (const bool checkpoint : {false, true}) {
        auto fs = NewFs();
        DatabaseOptions o = Options(fs);
        o.storage.checkpoint_on_close = checkpoint;
        std::string dump;
        {
            auto db = Open(fs, o);
            Connection conn(*db);
            Exec(conn, "CREATE TABLE big (id BIGINT, name VARCHAR, price DOUBLE, d DATE, flag "
                       "BOOLEAN, n INTEGER)");
            Exec(conn, "INSERT INTO big VALUES (1, 'seed', 0.25, DATE '2001-01-01', true, 5), "
                       "(2, NULL, NULL, NULL, NULL, NULL)");
            Grow(conn, "big", 11000);
            dump = Dump(*db);
            const auto snap = db->catalog().GetTable("big")->Snapshot();
            EXPECT_GE(snap->row_group_count(), 3U);
        }
        auto db = Open(fs, o);
        EXPECT_EQ(db->storage()->recovery().had_checkpoint, checkpoint);
        EXPECT_EQ(Dump(*db), dump) << (checkpoint ? "from the checkpoint" : "from the log");
        // and the restored table keeps working
        Connection conn(*db);
        Exec(conn, "INSERT INTO big VALUES (-1, 'after', 1.0, DATE '2030-01-01', false, 0)");
        EXPECT_EQ(conn.Query("SELECT count(*) FROM big WHERE id = -1").GetValue(0, 0),
                  Value::BigInt(1));
    }
}

TEST(Persistence, FailedStatementsWriteNothingToTheLog) {
    auto fs = NewFs();
    auto db = Open(fs);
    Connection conn(*db);
    Exec(conn, "CREATE TABLE t (a INTEGER NOT NULL, b VARCHAR)");
    Exec(conn, "INSERT INTO t VALUES (1, 'x')");
    const auto wal_size = [&] {
        return fs->Contents(std::string(kDir) + "/wal-0000000000000000.log").size();
    };
    const size_t before = wal_size();
    EXPECT_FALSE(conn.Query("CREATE TABLE t (z INTEGER)").ok()) << "already exists";
    EXPECT_FALSE(conn.Query("CREATE TABLE u (a INTEGER, a INTEGER)").ok()) << "duplicate column";
    EXPECT_FALSE(conn.Query("DROP TABLE nothing").ok());
    EXPECT_FALSE(conn.Query("INSERT INTO t VALUES (NULL, 'violates NOT NULL')").ok());
    EXPECT_FALSE(conn.Query("INSERT INTO t VALUES (1, 'ok'), (NULL, 'but this row is not')").ok());
    EXPECT_FALSE(conn.Query("INSERT INTO t SELECT a + 2147483647, b FROM t").ok()) << "overflow";
    EXPECT_FALSE(conn.Query("INSERT INTO nowhere VALUES (1)").ok());
    EXPECT_FALSE(conn.Query("SELEKT 1").ok());
    EXPECT_TRUE(conn.Query("CREATE TABLE IF NOT EXISTS t (z INTEGER)").ok());
    EXPECT_TRUE(conn.Query("DROP TABLE IF EXISTS nothing").ok());
    EXPECT_TRUE(conn.Query("INSERT INTO t SELECT a, b FROM t WHERE a > 100").ok()) << "no rows";
    EXPECT_TRUE(conn.Query("SELECT * FROM t").ok());
    EXPECT_EQ(wal_size(), before) << "none of that reached the log";
    EXPECT_FALSE(db->storage()->failed());
    EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(1));
}

TEST(Persistence, CopyIsAtomicDurableAndSurvivesReopening) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("cdb_persist_copy_" + std::to_string(::getpid()) + ".csv");
    {
        std::ofstream out(path);
        for (int i = 0; i < 9000; i++) {
            out << i << ",name " << i << "," << (i % 7) * 0.5 << "\n";
        }
    }
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.max_frame_bytes = 4096; // the load becomes many frames of one transaction
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        Exec(conn, "CREATE TABLE c (id BIGINT, name VARCHAR, x DOUBLE)");
        Exec(conn, "COPY c FROM '" + path.string() + "'");
        EXPECT_EQ(conn.Query("SELECT count(*) FROM c").GetValue(0, 0), Value::BigInt(9000));
        // an invalid file leaves the table and the log alone
        std::ofstream bad(path.string() + ".bad");
        bad << "1,ok,1.0\n2,not a number,xyz,extra\n";
        bad.close();
        const size_t log = fs->Contents(std::string(kDir) + "/wal-0000000000000000.log").size();
        EXPECT_FALSE(conn.Query("COPY c FROM '" + path.string() + ".bad'").ok());
        EXPECT_EQ(fs->Contents(std::string(kDir) + "/wal-0000000000000000.log").size(), log);
        EXPECT_GT(log, 9000U * 8) << "the rows are in the log";
    }
    auto db = Open(fs, o);
    EXPECT_EQ(db->storage()->recovery().transactions, 2U) << "CREATE and one COPY";
    EXPECT_EQ(db->storage()->recovery().frames, 7U)
        << "CREATE, then the COPY as five data frames (one per full chunk) and its commit frame";
    Connection conn(*db);
    EXPECT_EQ(conn.Query("SELECT count(*), sum(id) FROM c").GetValue(1, 0),
              Value::BigInt(9000LL * 8999 / 2));
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".bad");
}

TEST(Persistence, AnInsertIntoATableThatWasDroppedOrReplacedFailsAndLogsNothing) {
    auto fs = NewFs();
    auto db = Open(fs);
    Connection conn(*db);
    Exec(conn, "CREATE TABLE t (a INTEGER)");
    const std::shared_ptr<Table> stale = db->catalog().GetTable("t");
    Exec(conn, "DROP TABLE t");
    Exec(conn, "CREATE TABLE t (a INTEGER)"); // same name, a different table
    auto staging =
        std::make_unique<Table>("insert_staging", stale->schema(), stale->row_group_size());
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    chunk.SetValue(0, 0, Value::Integer(1));
    chunk.SetCardinality(1);
    staging->Append(chunk);
    ExpectError([&] { db->CommitAppend(stale, std::move(staging)); }, ErrorCode::Catalog,
                "dropped or replaced");
    EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(0));
    // the same rule holds without storage
    Database mem(1);
    Connection mconn(mem);
    Exec(mconn, "CREATE TABLE t (a INTEGER)");
    const std::shared_ptr<Table> mem_stale = mem.catalog().GetTable("t");
    Exec(mconn, "DROP TABLE t");
    auto staging2 =
        std::make_unique<Table>("insert_staging", mem_stale->schema(), mem_stale->row_group_size());
    staging2->Append(chunk);
    ExpectError([&] { mem.CommitAppend(mem_stale, std::move(staging2)); }, ErrorCode::Catalog,
                "dropped or replaced");
}

TEST(Persistence, OnlyOneDatabaseObjectMayHaveADirectoryOpen) {
    auto fs = NewFs();
    auto first = Open(fs);
    ExpectError([&] { Open(fs); }, ErrorCode::Io, "in use");
    first.reset();
    EXPECT_NO_THROW(Open(fs));
}

TEST(Persistence, CaseInsensitiveNamesKeepTheirSpellingAcrossRestarts) {
    auto fs = NewFs();
    {
        auto db = Open(fs);
        Connection conn(*db);
        Exec(conn, "CREATE TABLE \"CamelCase\" (\"Col A\" INTEGER, b VARCHAR)");
        Exec(conn, "INSERT INTO camelcase VALUES (1, 'x')");
    }
    auto db = Open(fs);
    EXPECT_EQ(db->catalog().ListTables(), std::vector<std::string>{"CamelCase"});
    Connection conn(*db);
    EXPECT_EQ(conn.Query("SELECT \"Col A\" FROM CAMELCASE").GetValue(0, 0), Value::Integer(1));
}

TEST(Persistence, ManySessionsCommittingAtOnceAreAllDurable) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.threads = 4;
    o.storage.checkpoint_wal_bytes = 30000; // checkpoints happen while sessions commit
    std::string dump;
    {
        auto db = Open(fs, o);
        {
            Connection setup(*db);
            Exec(setup, "CREATE TABLE shared (who INTEGER, i INTEGER)");
            for (int t = 0; t < 4; t++) {
                Exec(setup, "CREATE TABLE own" + std::to_string(t) + " (i INTEGER, s VARCHAR)");
            }
        }
        std::vector<std::thread> threads;
        std::atomic<int> failures{0};
        for (int t = 0; t < 4; t++) {
            threads.emplace_back([&, t] {
                Connection conn(*db);
                for (int i = 0; i < 60; i++) {
                    if (!conn.Query("INSERT INTO shared VALUES (" + std::to_string(t) + ", " +
                                    std::to_string(i) + ")")
                             .ok() ||
                        !conn.Query("INSERT INTO own" + std::to_string(t) + " VALUES (" +
                                    std::to_string(i) + ", '" + std::string(50, 'x') + "')")
                             .ok()) {
                        failures++;
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        EXPECT_EQ(failures.load(), 0);
        EXPECT_GE(db->storage()->checkpoint_epoch(), 1U);
        EXPECT_EQ(db->storage()->last_checkpoint_error(), "");
        Connection check(*db);
        EXPECT_EQ(check.Query("SELECT count(*) FROM shared").GetValue(0, 0), Value::BigInt(240));
        dump = Dump(*db);
    }
    auto db = Open(fs, o);
    EXPECT_EQ(Dump(*db), dump);
    Connection conn(*db);
    for (int t = 0; t < 4; t++) {
        EXPECT_EQ(conn.Query("SELECT count(*) FROM shared WHERE who = " + std::to_string(t))
                      .GetValue(0, 0),
                  Value::BigInt(60));
    }
}

// ---------------------------------------------------------------------------------- damage

TEST(Persistence, ATornTailOfTheLogIsDiscardedAndLaterCommitsAreKept) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)",
                         "INSERT INTO t VALUES (2)"});
    }
    const std::string wal = std::string(kDir) + "/wal-0000000000000000.log";
    std::vector<uint8_t> bytes = fs->Contents(wal);
    bytes.resize(bytes.size() - 5); // the last commit is torn
    fs->SetContents(wal, bytes);
    {
        auto db = Open(fs, o);
        EXPECT_GT(db->storage()->recovery().discarded_bytes, 0U);
        EXPECT_EQ(db->storage()->recovery().transactions, 2U);
        Connection conn(*db);
        EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(1));
        Exec(conn, "INSERT INTO t VALUES (3)"); // appended right after the cut
    }
    auto db = Open(fs, o);
    EXPECT_EQ(db->storage()->recovery().discarded_bytes, 0U)
        << "the first recovery cut the tail off";
    Connection conn(*db);
    EXPECT_EQ(conn.Query("SELECT count(*), sum(a) FROM t").GetValue(1, 0), Value::BigInt(4));
}

TEST(Persistence, ACorruptCheckpointIsRefusedNotWorkedAround) {
    auto fs = NewFs();
    {
        auto db = Open(fs);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)", "CHECKPOINT",
                         "INSERT INTO t VALUES (2)"});
    }
    const std::string ckpt = std::string(kDir) + "/checkpoint-0000000000000002.cdb";
    ASSERT_TRUE(fs->Exists(ckpt));
    std::vector<uint8_t> bytes = fs->Contents(ckpt);
    bytes[bytes.size() / 2] ^= 0x10;
    fs->SetContents(ckpt, bytes);
    ExpectError([&] { Open(fs); }, ErrorCode::Corruption);
    bytes[bytes.size() / 2] ^= 0x10;
    fs->SetContents(ckpt, bytes);
    EXPECT_NO_THROW(Open(fs)) << "and once repaired it opens";
}

TEST(Persistence, AMissingLogOrAGapInTheChainIsCorruption) {
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "CHECKPOINT", "INSERT INTO t VALUES (1)"});
    }
    // the log that follows the checkpoint disappears
    auto lost = fs->Crash(CrashPolicy::KeepAll());
    lost->Remove(std::string(kDir) + "/wal-0000000000000001.log");
    ExpectError([&] { Open(lost); }, ErrorCode::Corruption, "is missing");
    // a database that only has a log, but not the first one
    auto no_first = fs->Crash(CrashPolicy::KeepAll());
    no_first->Remove(std::string(kDir) + "/checkpoint-0000000000000001.cdb");
    ExpectError([&] { Open(no_first); }, ErrorCode::Corruption, "is missing");
}

TEST(Persistence, AnOlderLogThatIsDamagedWhileANewerOneExistsIsCorruption) {
    // a power cut during a checkpoint leaves the old checkpoint, the old log and the new log
    auto fs = NewFs();
    DatabaseOptions o = Options(fs);
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)"});
    }
    auto crashed = fs->Crash(CrashPolicy::KeepAll());
    // plant a "next" log by hand, as a checkpoint's rotation would have
    auto wal1 = WalWriter::Create(*crashed, std::string(kDir) + "/wal-0000000000000001.log", 1);
    (void)wal1;
    EXPECT_NO_THROW(Open(crashed, o)) << "the older log is complete, so recovery chains through it";
    // now damage the older log's tail
    auto damaged = fs->Crash(CrashPolicy::KeepAll());
    auto w1 = WalWriter::Create(*damaged, std::string(kDir) + "/wal-0000000000000001.log", 1);
    (void)w1;
    std::vector<uint8_t> bytes = damaged->Contents(std::string(kDir) + "/wal-0000000000000000.log");
    bytes.resize(bytes.size() - 3);
    damaged->SetContents(std::string(kDir) + "/wal-0000000000000000.log", bytes);
    ExpectError([&] { Open(damaged, o); }, ErrorCode::Corruption,
                "damaged, but a later log exists");
}

TEST(Persistence, StaleFilesAreCleanedUpWhenTheDatabaseOpens) {
    auto fs = NewFs();
    {
        auto db = Open(fs);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "CHECKPOINT", "INSERT INTO t VALUES (1)"});
    }
    // leftovers of an interrupted checkpoint and of older epochs
    fs->SetContents(std::string(kDir) + "/checkpoint-0000000000000009.cdb.tmp", {1, 2, 3});
    fs->SetContents(std::string(kDir) + "/wal-0000000000000000.log", {9, 9, 9});
    fs->SetContents(std::string(kDir) + "/checkpoint-0000000000000000.cdb", {9, 9, 9});
    fs->SetContents(std::string(kDir) + "/notes.txt", {'h', 'i'});
    auto db = Open(fs);
    EXPECT_FALSE(Has(*fs, "checkpoint-0000000000000009.cdb.tmp"));
    EXPECT_FALSE(Has(*fs, "wal-0000000000000000.log"));
    EXPECT_FALSE(Has(*fs, "checkpoint-0000000000000000.cdb"));
    EXPECT_TRUE(Has(*fs, "notes.txt")) << "files that are not the database's are left alone";
    EXPECT_EQ(db->catalog().ListTables(), std::vector<std::string>{"t"});
}

// ---- a log that checksums correctly but could not have been written by this database ------------

namespace {

// payload bytes of the log operations (the format of ADR 0009)
std::vector<uint8_t> CreateOp(const std::string& name,
                              const std::vector<std::pair<std::string, TypeId>>& columns,
                              bool not_null = false, uint64_t row_group_size = 2 * kVectorSize) {
    BinaryWriter w;
    w.U8(1);
    w.String(name);
    w.U32(static_cast<uint32_t>(columns.size()));
    for (const auto& [column, type] : columns) {
        w.String(column);
        w.U8(static_cast<uint8_t>(type));
        w.U8(not_null ? 1 : 0);
    }
    w.U64(row_group_size);
    return w.Take();
}
std::vector<uint8_t> DropOp(const std::string& name) {
    BinaryWriter w;
    w.U8(2);
    w.String(name);
    return w.Take();
}
std::vector<uint8_t> AppendOp(const std::string& name, const std::vector<LogicalType>& types,
                              const std::vector<Value>& row) {
    BinaryWriter w;
    w.U8(3);
    w.String(name);
    DataChunk chunk;
    chunk.Initialize(types);
    for (idx_t c = 0; c < types.size(); c++) {
        chunk.SetValue(c, 0, row[c]);
    }
    chunk.SetCardinality(1);
    WriteChunk(w, chunk);
    return w.Take();
}

// Writes wal-0 with one committed frame per payload and returns the file system.
std::shared_ptr<MemoryFileSystem> LogWith(const std::vector<std::vector<uint8_t>>& payloads) {
    auto fs = NewFs();
    fs->CreateDirectories(kDir);
    auto wal = WalWriter::Create(*fs, std::string(kDir) + "/wal-0000000000000000.log", 0);
    for (const auto& p : payloads) {
        wal->AppendFrame(p, true);
    }
    wal->Sync();
    fs->SyncDirectory(kDir);
    return fs;
}

} // namespace

TEST(Persistence, ALogThatCouldNotHaveBeenWrittenByThisDatabaseIsCorruption) {
    const std::vector<std::pair<std::string, TypeId>> one_int = {{"a", TypeId::Integer}};
    const std::vector<LogicalType> int_type = {LogicalType::Integer()};
    // sanity: a well-formed hand-made log opens
    {
        auto fs = LogWith({CreateOp("t", one_int), AppendOp("t", int_type, {Value::Integer(7)})});
        auto db = Open(fs);
        Connection conn(*db);
        EXPECT_EQ(conn.Query("SELECT a FROM t").GetValue(0, 0), Value::Integer(7));
    }
    const auto expect_corrupt = [&](const std::string& what,
                                    const std::vector<std::vector<uint8_t>>& payloads,
                                    const std::string& message = "") {
        auto fs = LogWith(payloads);
        ExpectError([&] { Open(fs); }, ErrorCode::Corruption, message);
        (void)what;
    };
    expect_corrupt("creating a table that exists", {CreateOp("t", one_int), CreateOp("T", one_int)},
                   "already exists");
    expect_corrupt("dropping a table that does not exist", {DropOp("nothing")}, "does not exist");
    expect_corrupt("appending to an unknown table",
                   {AppendOp("nowhere", int_type, {Value::Integer(1)})}, "unknown table");
    expect_corrupt("an unknown operation", {{9, 1, 2, 3}}, "unknown operation");
    expect_corrupt("a table without columns", {CreateOp("t", {})}, "at least one column");
    expect_corrupt("duplicate column names",
                   {CreateOp("t", {{"a", TypeId::Integer}, {"A", TypeId::Integer}})});
    expect_corrupt("a column of an unknown type", {CreateOp("t", {{"a", static_cast<TypeId>(77)}})},
                   "invalid column");
    expect_corrupt("a row group size of 5", {CreateOp("t", one_int, false, 5)}, "row group size");
    expect_corrupt("a NULL in a NOT NULL column",
                   {CreateOp("t", one_int, true),
                    AppendOp("t", int_type, {Value::Null(LogicalType::Integer())})});
    expect_corrupt("an append whose rows do not match the table",
                   {CreateOp("t", {{"a", TypeId::Integer}, {"b", TypeId::Integer}}),
                    AppendOp("t", int_type, {Value::Integer(1)})});
    expect_corrupt("an operation cut short", {[&] {
                       std::vector<uint8_t> p = CreateOp("t", one_int);
                       p.resize(p.size() - 3);
                       return p;
                   }()});
    // dropping and re-creating is fine, and an empty frame says nothing
    auto fs =
        LogWith({CreateOp("t", one_int), DropOp("t"), CreateOp("t", {{"b", TypeId::Varchar}}), {}});
    auto db = Open(fs);
    EXPECT_EQ(db->catalog().GetTable("t")->schema()[0].name, "b");
}

TEST(Persistence, LogsThatSkipAnEpochOrBelongToAnotherAreCorruption) {
    // wal-0 and wal-2 without wal-1: a hole in the chain
    auto fs = LogWith({});
    auto wal2 = WalWriter::Create(*fs, std::string(kDir) + "/wal-0000000000000002.log", 2);
    wal2->Sync();
    ExpectError([&] { Open(fs); }, ErrorCode::Corruption, "wal-0000000000000001.log");
    // a log whose header says another epoch than its name
    auto other = NewFs();
    other->CreateDirectories(kDir);
    auto w = WalWriter::Create(*other, std::string(kDir) + "/wal-0000000000000000.log", 5);
    w->Sync();
    ExpectError([&] { Open(other); }, ErrorCode::Corruption, "epoch");
}

// ---------------------------------------------------------------------------------- failing disks

TEST(Persistence, AFailedLogWriteFailsTheStatementAndMakesTheDatabaseReadOnly) {
    for (const bool fail_fsync : {false, true}) {
        auto mem = NewFs();
        auto fi = std::make_shared<FaultInjector>(mem);
        DatabaseOptions o = Options(fi);
        o.storage.checkpoint_on_close = false;
        {
            auto db = Open(fi, o);
            Connection conn(*db);
            RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)"});
            const uint64_t ops = fi->operations();
            fi->FailOperation(fail_fsync ? ops + 1 : ops); // the write, or the fsync after it
            const QueryResult bad = conn.Query("INSERT INTO t VALUES (2)");
            ASSERT_FALSE(bad.ok());
            EXPECT_EQ(bad.error_code(), ErrorCode::Io);
            EXPECT_TRUE(db->storage()->failed());
            EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(1))
                << "the rows of the failed statement were never applied";
            // from now on every change is refused, with the reason; reads still work
            for (const char* sql : {"INSERT INTO t VALUES (3)", "CREATE TABLE u (x INTEGER)",
                                    "DROP TABLE t", "CHECKPOINT"}) {
                const QueryResult r = conn.Query(sql);
                ASSERT_FALSE(r.ok()) << sql;
                EXPECT_NE(r.error_message().find("read-only"), std::string::npos)
                    << r.error_message();
            }
            EXPECT_TRUE(conn.Query("SELECT * FROM t").ok());
        }
        // what comes back is a committed prefix: both rows or just the first (the failed commit may
        // or may not have reached the disk), never anything else
        auto after = Open(mem, Options(mem));
        Connection conn(*after);
        const auto count = conn.Query("SELECT count(*) FROM t").GetValue(0, 0).GetBigInt();
        EXPECT_TRUE(count == 1 || count == 2) << count;
    }
}

TEST(Persistence, AFailureWhileSwitchingLogsAlsoMakesTheDatabaseReadOnly) {
    auto mem = NewFs();
    auto fi = std::make_shared<FaultInjector>(mem);
    DatabaseOptions o = Options(fi);
    o.storage.checkpoint_on_close = false;
    auto db = Open(fi, o);
    Connection conn(*db);
    RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)"});
    fi->FailOperation(
        fi->operations() +
        1); // sync of the old log is op 0 of the checkpoint; creating the new one is next
    EXPECT_FALSE(conn.Query("CHECKPOINT").ok());
    EXPECT_TRUE(db->storage()->failed());
    EXPECT_FALSE(conn.Query("INSERT INTO t VALUES (2)").ok());
    EXPECT_EQ(conn.Query("SELECT count(*) FROM t").GetValue(0, 0), Value::BigInt(1));
}

TEST(Persistence, AFailedCheckpointFileLeavesTheDatabaseUsableAndRecoverable) {
    auto mem = NewFs();
    auto fi = std::make_shared<FaultInjector>(mem);
    DatabaseOptions o = Options(fi);
    o.storage.checkpoint_on_close = false;
    {
        auto db = Open(fi, o);
        Connection conn(*db);
        RunScript(conn, {"CREATE TABLE t (a INTEGER)", "INSERT INTO t VALUES (1)"});
        // fail somewhere inside writing the checkpoint file (after the new log exists)
        fi->FailOperation(fi->operations() + 5);
        EXPECT_FALSE(conn.Query("CHECKPOINT").ok());
        EXPECT_FALSE(db->storage()->failed())
            << "the log chain is intact; only the snapshot is missing";
        Exec(conn, "INSERT INTO t VALUES (2)");
        Exec(conn, "CHECKPOINT"); // and a retry works
        Exec(conn, "INSERT INTO t VALUES (3)");
    }
    auto db = Open(mem, Options(mem));
    Connection conn(*db);
    EXPECT_EQ(conn.Query("SELECT count(*), sum(a) FROM t").GetValue(1, 0), Value::BigInt(6));
}

// ---------------------------------------------------------------------------------- durability
// levels

TEST(Persistence, EveryAcknowledgedStatementSurvivesALostPageCache) {
    auto fs = NewFs();
    auto db = Open(fs);
    Connection conn(*db);
    RunScript(conn, Script());
    const std::string expected = Dump(*db);
    // the machine loses power right now: only what was fsynced survives
    auto after = fs->Crash(CrashPolicy::DropUnsynced());
    auto recovered = Open(after);
    EXPECT_EQ(Dump(*recovered), expected);
}

TEST(Persistence, WithoutFsyncAPowerCutLosesAtMostTheLastStatementsButNeverCorrupts) {
    DatabaseOptions base;
    base.storage.sync = SyncMode::Off;
    base.storage.checkpoint_on_close = false;
    std::vector<std::string> prefixes;
    {
        Database ref(1);
        Connection rconn(ref);
        prefixes.push_back(Dump(ref));
        for (const std::string& sql : Script()) {
            Exec(rconn, sql);
            prefixes.push_back(Dump(ref));
        }
    }
    auto fs = NewFs();
    {
        DatabaseOptions o = Options(fs);
        o.storage = base.storage;
        o.storage.fs = fs;
        auto db = Open(fs, o);
        Connection conn(*db);
        RunScript(conn, Script());
        // nothing was fsynced after the log was created: the file system has not been asked to
        // persist a single commit
        EXPECT_GT(fs->UnsyncedOperations(), 0U);
        for (uint64_t seed = 0; seed < 60; seed++) {
            const auto crashed =
                fs->Crash(seed % 3 == 0 ? CrashPolicy::DropUnsynced()
                                        : CrashPolicy::Random(seed, seed % 2 == 0));
            auto recovered = Open(crashed, Options(crashed));
            const std::string got = Dump(*recovered);
            EXPECT_TRUE(std::find(prefixes.begin(), prefixes.end(), got) != prefixes.end())
                << "seed " << seed << ": recovered a state that no prefix of the script produces";
        }
    }
}

// ---------------------------------------------------------------------------------- the real disk

TEST(Persistence, ARealDirectoryOnDiskWorksTheSameWay) {
    const auto dir =
        std::filesystem::temp_directory_path() / ("cdb_persist_dir_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    {
        Database db(dir.string(), [] {
            DatabaseOptions o;
            o.threads = 2;
            o.row_group_size = 2 * kVectorSize;
            return o;
        }());
        Connection conn(db);
        RunScript(conn, Script());
        EXPECT_TRUE(std::filesystem::exists(dir / "LOCK"));
        ExpectError([&] { Database second(dir.string()); }, ErrorCode::Io, "in use");
    }
    EXPECT_TRUE(std::filesystem::exists(dir / "checkpoint-0000000000000001.cdb"));
    {
        Database db(dir.string());
        EXPECT_TRUE(db.storage()->recovery().had_checkpoint);
        EXPECT_EQ(Dump(db), ReferenceDump(Script()));
        Connection conn(db);
        Exec(conn, "INSERT INTO b VALUES (12345)");
    }
    {
        Database db(dir.string());
        EXPECT_EQ(db.storage()->recovery().transactions, 0U)
            << "closed cleanly: folded into a checkpoint";
        Connection conn(db);
        EXPECT_EQ(conn.Query("SELECT count(*) FROM b WHERE k = 12345").GetValue(0, 0),
                  Value::BigInt(1));
    }
    std::filesystem::remove_all(dir);
}

// Not a test: when CDB_WRITE_FUZZ_SEEDS names a directory, writes the seed corpora of the
// checkpoint and WAL fuzz targets there (real files of a small database), the way fuzz/corpus/ was
// made.
TEST(Persistence, WritesFuzzSeedsWhenAsked) {
    const char* dir = std::getenv("CDB_WRITE_FUZZ_SEEDS");
    if (dir == nullptr) {
        GTEST_SKIP() << "set CDB_WRITE_FUZZ_SEEDS=<dir> to regenerate the fuzz seeds";
    }
    const auto write = [&](const std::string& target, const std::string& name,
                           const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(std::filesystem::path(dir) / target);
        std::ofstream out(std::filesystem::path(dir) / target / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    };
    for (const bool tiny : {true, false}) {
        auto fs = NewFs();
        DatabaseOptions o = Options(fs);
        o.storage.checkpoint_on_close = false;
        const std::vector<std::string> script =
            tiny ? std::vector<std::string>{"CREATE TABLE t (a INTEGER, s VARCHAR)",
                                            "INSERT INTO t VALUES (1, 'x'), (2, NULL)"}
                 : Script();
        {
            auto db = Open(fs, o);
            Connection conn(*db);
            RunScript(conn, script);
        }
        const std::string tag = tiny ? "tiny" : "mixed";
        std::vector<uint8_t> wal = fs->Contents(std::string(kDir) + "/wal-0000000000000000.log");
        write("wal", tag + "_log_only", [&] {
            std::vector<uint8_t> v = {1}; // mode 1: the harness supplies the header
            v.insert(v.end(), wal.begin() + 16, wal.end());
            return v;
        }());
        write("wal", tag + "_with_header", [&] {
            std::vector<uint8_t> v = {0}; // mode 0: the bytes are the whole file
            v.insert(v.end(), wal.begin(), wal.end());
            return v;
        }());
        // mode 2: the frames' payloads (operation streams) as one frame, which the harness
        // checksums
        {
            const WalScan scan = ScanWal(*fs, std::string(kDir) + "/wal-0000000000000000.log", 0);
            std::vector<uint8_t> v = {2};
            ReplayWal(*fs, std::string(kDir) + "/wal-0000000000000000.log", scan,
                      [&](const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); });
            write("wal", tag + "_operations", v);
        }
        {
            auto db = Open(fs, o);
            db->Checkpoint();
        }
        write("checkpoint", tag,
              fs->Contents(std::string(kDir) + "/checkpoint-0000000000000001.cdb"));
    }
}

} // namespace cdb
