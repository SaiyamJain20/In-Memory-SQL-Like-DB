// kill -9 on a real process writing to a real disk: the child commits rows one statement at a time
// (each fsynced) and reports every acknowledgement through a pipe; the parent kills it at a random
// moment - mid-statement, mid-checkpoint, mid-anything - and reopens the directory. What must be
// there: every row the child was told about, in order, and at most the one that was in flight.

#include "main/connection.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cdb {

namespace {

DatabaseOptions KillOptions() {
    DatabaseOptions o;
    o.threads = 1;
    o.row_group_size = 2 * kVectorSize;
    o.storage.checkpoint_wal_bytes = 3000; // so the kills also land in checkpoints
    return o;
}

[[noreturn]] void ChildMain(const std::string& dir, int fd) {
    try {
        Database db(dir, KillOptions());
        Connection conn(db);
        if (!conn.Query("CREATE TABLE t (id BIGINT, s VARCHAR)").ok()) {
            _exit(3);
        }
        for (int64_t i = 0;; i++) {
            const QueryResult r =
                conn.Query("INSERT INTO t VALUES (" + std::to_string(i) +
                           ", 'row number ' || CAST(" + std::to_string(i) + " AS VARCHAR))");
            if (!r.ok()) {
                _exit(4);
            }
            if (write(fd, &i, sizeof(i)) != static_cast<ssize_t>(sizeof(i))) {
                _exit(5);
            }
        }
    } catch (...) {
        _exit(6);
    }
}

} // namespace

TEST(Kill9, AKilledWriterLeavesEveryAcknowledgedRowAndNothingElse) {
#if defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "ThreadSanitizer does not support fork() in a process that has used threads";
#endif
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("cdb_kill9_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::mt19937 rng(12345);
    int64_t total_acked = 0;
    int kills_in_checkpoints_or_later = 0;
    for (int round = 0; round < 25; round++) {
        const std::string dir = (root / std::to_string(round)).string();
        int fds[2];
        ASSERT_EQ(pipe(fds), 0);
        const pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            close(fds[0]);
            ChildMain(dir, fds[1]);
        }
        close(fds[1]);
        // let the child get going whatever the machine's load (at least 100 acknowledged rows, which
        // is several checkpoints at this threshold), then kill it at a random moment
        int64_t acked = 0, last = -1;
        while (acked < 100 &&
               read(fds[0], &last, sizeof(last)) == static_cast<ssize_t>(sizeof(last))) {
            acked = last + 1;
        }
        usleep(static_cast<useconds_t>(rng() % 60000));
        kill(pid, SIGKILL);
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
            << "the child ended on its own (status " << status << "): it must be killed";
        while (read(fds[0], &last, sizeof(last)) == static_cast<ssize_t>(sizeof(last))) {
            acked = last + 1;
        }
        close(fds[0]);
        total_acked += acked;

        // the dead process's lock is gone with it, so the directory opens again
        Database db(dir, KillOptions());
        if (db.catalog().ListTables().empty()) {
            EXPECT_EQ(acked, 0) << "round " << round << ": the table was acknowledged and is gone";
            continue;
        }
        Connection conn(db);
        const QueryResult r = conn.Query("SELECT id, s FROM t");
        ASSERT_TRUE(r.ok());
        const auto rows = static_cast<int64_t>(r.RowCount());
        EXPECT_GE(rows, acked) << "round " << round << ": an acknowledged row was lost";
        EXPECT_LE(rows, acked + 1) << "round " << round << ": rows nobody asked for";
        for (idx_t i = 0; i < r.RowCount(); i++) {
            ASSERT_EQ(r.GetValue(0, i), Value::BigInt(static_cast<int64_t>(i)))
                << "round " << round << " row " << i;
            ASSERT_EQ(r.GetValue(1, i), Value::Varchar("row number " + std::to_string(i)));
        }
        if (db.storage()->recovery().had_checkpoint) {
            kills_in_checkpoints_or_later++;
        }
        // and it carries on from there
        ASSERT_TRUE(conn.Query("INSERT INTO t VALUES (-1, 'after recovery')").ok());
    }
    EXPECT_GE(total_acked, 25 * 100) << "the child committed at least 100 rows before every kill";
    EXPECT_GT(kills_in_checkpoints_or_later, 20)
        << "nearly every recovery starts from a checkpoint";
    std::filesystem::remove_all(root);
}

} // namespace cdb
