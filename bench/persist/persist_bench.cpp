// cdb_persist: measures what durability costs and what it buys (Phase 7).
//
//   cdb_persist --dir DIR [--sf 1] [--threads N] [--commits N]
//
// DIR must be on the disk being measured (not tmpfs, where fsync is free). Phases:
//   1. commit latency of single-row INSERTs: fsync per statement, no fsync, and in memory;
//   2. TPC-H load into a persistent database (COPY, every row logged and fsynced) against the same
//      load in memory;
//   3. CHECKPOINT: time and file size;
//   4. reopen from the checkpoint, against re-loading the CSV files;
//   5. recovery that must replay the whole log (no checkpoint was taken).
// Needs the TPC-H data of tools/tpch_data.py for the chosen scale factor.

#include "main/connection.h"
#include "main/database.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double Seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

long MaxRssMb() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

double DirMb(const fs::path& dir, const std::string& prefix = "") {
    double bytes = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().filename().string().rfind(prefix, 0) == 0) {
            bytes += static_cast<double>(e.file_size());
        }
    }
    return bytes / 1048576.0;
}

const char* const kTables[] = {"nation",   "region",   "part",   "supplier",
                               "partsupp", "customer", "orders", "lineitem"};

void Fail(const std::string& what) {
    std::cerr << what << "\n";
    std::exit(1);
}

void Check(const cdb::QueryResult& r, const std::string& what) {
    if (!r.ok()) {
        Fail(what + ": " + r.error_message());
    }
}

// Single-row INSERT statements; returns sorted per-statement latencies in microseconds.
std::vector<double> CommitLatencies(cdb::Database& db, int n) {
    cdb::Connection conn(db);
    Check(conn.Query("CREATE TABLE commits (id BIGINT, payload VARCHAR)"), "create");
    std::vector<double> us;
    us.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const auto start = Clock::now();
        Check(conn.Query("INSERT INTO commits VALUES (" + std::to_string(i) +
                         ", 'a payload of about thirty bytes')"),
              "insert");
        us.push_back(Seconds(Clock::now() - start) * 1e6);
    }
    std::sort(us.begin(), us.end());
    return us;
}

void PrintLatencies(const char* label, const std::vector<double>& us) {
    double total = 0;
    for (const double u : us) {
        total += u;
    }
    std::printf("  %-34s %9.0f stmts/s   mean %8.1f us   p50 %8.1f   p99 %8.1f   max %9.1f\n",
                label, static_cast<double>(us.size()) / (total / 1e6),
                total / static_cast<double>(us.size()), us[us.size() / 2], us[us.size() * 99 / 100],
                us.back());
}

std::string DataDir(double sf) {
    char sf_text[32];
    std::snprintf(sf_text, sizeof sf_text, "%g", sf);
    return std::string(CDB_SOURCE_DIR) + "/data/tpch-sf" + sf_text;
}

double LoadTpch(cdb::Database& db, const std::string& data) {
    cdb::Connection conn(db);
    Check(conn.Query(ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/schema.sql")), "schema");
    const auto start = Clock::now();
    for (const char* t : kTables) {
        Check(conn.Query(std::string("COPY ") + t + " FROM '" + data + "/" + t +
                         ".csv' (DELIMITER '|', HEADER FALSE)"),
              std::string("loading ") + t);
    }
    return Seconds(Clock::now() - start);
}

size_t TotalRows(cdb::Database& db) {
    size_t rows = 0;
    for (const char* t : kTables) {
        rows += db.catalog().GetTable(t)->RowCount();
    }
    return rows;
}

cdb::DatabaseOptions Options(size_t threads, cdb::SyncMode sync, bool checkpoint_on_close) {
    cdb::DatabaseOptions o;
    o.threads = threads;
    o.storage.sync = sync;
    o.storage.checkpoint_on_close = checkpoint_on_close;
    o.storage.checkpoint_wal_bytes = 0; // only when asked
    return o;
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    double sf = 1;
    size_t threads = 0;
    int commits = 2000;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--sf" && i + 1 < argc) {
            sf = std::atof(argv[++i]);
        } else if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (a == "--commits" && i + 1 < argc) {
            commits = std::atoi(argv[++i]);
        } else {
            std::cerr << "usage: cdb_persist --dir DIR [--sf N] [--threads N] [--commits N]\n";
            return 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "usage: cdb_persist --dir DIR [--sf N] [--threads N] [--commits N]\n";
        return 2;
    }
    const std::string data = DataDir(sf);
    if (!fs::exists(data + "/manifest.json")) {
        Fail("no TPC-H data at " + data + " (run tools/tpch_data.py --sf " + std::to_string(sf) +
             ")");
    }
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::printf("cdb_persist: TPC-H SF%g, %zu thread(s), data on %s\n\n", sf,
                threads == 0 ? cdb::TaskScheduler::HardwareThreads() : threads, dir.c_str());

    std::printf("1. commit latency, single-row INSERT\n");
    {
        cdb::Database db((fs::path(dir) / "commit_full").string(),
                         Options(1, cdb::SyncMode::Full, false));
        PrintLatencies("fsync per statement (Full)", CommitLatencies(db, commits));
    }
    {
        cdb::Database db((fs::path(dir) / "commit_off").string(),
                         Options(1, cdb::SyncMode::Off, false));
        PrintLatencies("no fsync (Off)", CommitLatencies(db, commits * 10));
    }
    {
        cdb::Database db(1);
        PrintLatencies("in memory", CommitLatencies(db, commits * 10));
    }

    std::printf("\n2. loading TPC-H SF%g with COPY\n", sf);
    double memory_load;
    {
        cdb::Database db(threads);
        memory_load = LoadTpch(db, data);
        std::printf("  in memory                            %8.2f s   (%zu rows)\n", memory_load,
                    TotalRows(db));
    }
    const fs::path main_dir = fs::path(dir) / "tpch";
    double persistent_load;
    {
        cdb::DatabaseOptions o = Options(threads, cdb::SyncMode::Full, false);
        cdb::Database db(main_dir.string(), o);
        persistent_load = LoadTpch(db, data);
        std::printf("  persistent, every row logged+fsynced %8.2f s   log %.1f MB\n",
                    persistent_load, DirMb(main_dir, "wal-"));

        std::printf("\n3. CHECKPOINT\n");
        cdb::Connection conn(db);
        const auto start = Clock::now();
        Check(conn.Query("CHECKPOINT"), "checkpoint");
        const double t = Seconds(Clock::now() - start);
        std::printf("  write checkpoint                     %8.2f s   file %.1f MB (stored data in "
                    "memory: see cdb_tpch)\n",
                    t, DirMb(main_dir, "checkpoint-"));
    }

    std::printf("\n4. reopen from the checkpoint\n");
    {
        const auto start = Clock::now();
        cdb::Database db(main_dir.string(), Options(threads, cdb::SyncMode::Full, false));
        const double t = Seconds(Clock::now() - start);
        const cdb::RecoveryStats& r = db.storage()->recovery();
        std::printf("  open + load all tables               %8.2f s   (%zu tables, %zu rows, %llu "
                    "log statements replayed)   vs %.2f s to COPY the CSV files\n",
                    t, r.tables, TotalRows(db), static_cast<unsigned long long>(r.transactions),
                    memory_load);
        cdb::Connection conn(db);
        const auto q = Clock::now();
        Check(conn.Query(ReadFile(std::string(CDB_SOURCE_DIR) + "/bench/tpch/queries/q06.sql")),
              "q6");
        std::printf("  Q6 on the reopened database          %8.2f ms\n",
                    Seconds(Clock::now() - q) * 1000);
        std::printf("  peak RSS so far: %ld MB\n", MaxRssMb());
    }

    std::printf("\n5. recovery that replays the whole log (no checkpoint)\n");
    const fs::path log_dir = fs::path(dir) / "tpch_log_only";
    {
        cdb::Database db(log_dir.string(), Options(threads, cdb::SyncMode::Full, false));
        LoadTpch(db, data);
    }
    {
        const auto start = Clock::now();
        cdb::Database db(log_dir.string(), Options(threads, cdb::SyncMode::Full, false));
        const double t = Seconds(Clock::now() - start);
        const cdb::RecoveryStats& r = db.storage()->recovery();
        std::printf("  open + replay %.1f MB of log         %8.2f s   (%llu statements, %llu "
                    "frames, %zu rows)\n",
                    DirMb(log_dir, "wal-"), t, static_cast<unsigned long long>(r.transactions),
                    static_cast<unsigned long long>(r.frames), TotalRows(db));
    }
    fs::remove_all(dir);
    return 0;
}
