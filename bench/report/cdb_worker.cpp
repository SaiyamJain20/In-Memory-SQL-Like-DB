// cdb_report_worker: the cdb side of the comparison harness (bench/report/driver.py).
//
// A line protocol on stdin / stdout, so that cdb is driven exactly like the Python workers of the
// other engines (bench/report/py_worker.py): one request per line, fields separated by tabs, one
// JSON object per reply line.
//
//   THREADS <n>               open an in-memory database that may use n threads (0: all)
//   EXEC <sql file>           run the statements in the file (schema, COPY, INSERT, ...)
//   RUN <sql file> <runs>     run the query `runs` times; report wall and CPU milliseconds per run
//   DUMP <sql file> <csv>     run the query once and write its rows ('|' separated, doubles with 17
//                             significant digits, NULL as an empty field) for the correctness check
//   STORED <t1,t2,...>        bytes the columnar storage of those tables holds
//   STAT                      peak and current resident set, thread count of the process
//   QUIT
//
// Timing is Connection::Query end to end (parse, bind, optimize, plan, execute, materialise the
// result), the same quantity bench/tpch/tpch_runner.cpp reports.

#include "main/connection.h"
#include "main/database.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double CpuMs() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 +
           static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
}

std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += ' ';
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

void Reply(const std::string& body) {
    std::printf("{%s}\n", body.c_str());
    std::fflush(stdout);
}

void Fail(const std::string& message) {
    Reply("\"ok\":false,\"error\":" + JsonString(message));
}

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cell;
    std::stringstream ss(line);
    while (std::getline(ss, cell, sep)) {
        out.push_back(cell);
    }
    return out;
}

// One field of a '|' separated file: quoted when it holds the separator, a quote or a newline.
void WriteCell(std::ofstream& out, const std::string& text) {
    if (text.find_first_of("|\"\n") == std::string::npos) {
        out << text;
        return;
    }
    out << '"';
    for (const char c : text) {
        if (c == '"') {
            out << '"';
        }
        out << c;
    }
    out << '"';
}

// "/proc/self/status" line value in kB.
long StatusKb(const char* key) {
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind(key, 0) == 0) {
            return std::atol(line.c_str() + std::string(key).size());
        }
    }
    return -1;
}

} // namespace

int main() {
    std::unique_ptr<cdb::Database> db;
    std::unique_ptr<cdb::Connection> conn;
    for (std::string line; std::getline(std::cin, line);) {
        const std::vector<std::string> f = Split(line, '\t');
        if (f.empty()) {
            continue;
        }
        const std::string& op = f[0];
        if (op == "QUIT") {
            Reply("\"ok\":true");
            return 0;
        }
        if (op == "THREADS" && f.size() >= 2) {
            db = std::make_unique<cdb::Database>(static_cast<size_t>(std::atoi(f[1].c_str())));
            conn = std::make_unique<cdb::Connection>(*db);
            Reply("\"ok\":true,\"threads\":" + std::to_string(db->threads()));
            continue;
        }
        if (op == "STAT") {
            Reply("\"ok\":true,\"vm_hwm_kb\":" + std::to_string(StatusKb("VmHWM:")) +
                  ",\"vm_rss_kb\":" + std::to_string(StatusKb("VmRSS:")) +
                  ",\"os_threads\":" + std::to_string(StatusKb("Threads:")));
            continue;
        }
        if (conn == nullptr) {
            Fail("send THREADS first");
            continue;
        }
        if (op == "STORED" && f.size() >= 2) {
            size_t bytes = 0;
            for (const std::string& t : Split(f[1], ',')) {
                bytes += db->catalog().GetTable(t)->MemoryUsage();
            }
            Reply("\"ok\":true,\"bytes\":" + std::to_string(bytes));
            continue;
        }
        if (op == "EXEC" && f.size() >= 2) {
            const std::string sql = ReadFile(f[1]);
            const auto start = std::chrono::steady_clock::now();
            const double cpu0 = CpuMs();
            const cdb::QueryResult r = conn->Query(sql);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (!r.ok()) {
                Fail(r.error_message());
                continue;
            }
            Reply("\"ok\":true,\"ms\":" + std::to_string(ms) +
                  ",\"cpu_ms\":" + std::to_string(CpuMs() - cpu0));
            continue;
        }
        if (op == "RUN" && f.size() >= 3) {
            const std::string sql = ReadFile(f[1]);
            const int runs = std::atoi(f[2].c_str());
            std::string wall = "[", cpu = "[";
            size_t rows = 0;
            bool failed = false;
            for (int i = 0; i < runs; i++) {
                const auto start = std::chrono::steady_clock::now();
                const double cpu0 = CpuMs();
                const cdb::QueryResult r = conn->Query(sql);
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
                const double cpu_ms = CpuMs() - cpu0;
                if (!r.ok()) {
                    Fail(r.error_message());
                    failed = true;
                    break;
                }
                rows = r.RowCount();
                wall += (i ? "," : "") + std::to_string(ms);
                cpu += (i ? "," : "") + std::to_string(cpu_ms);
            }
            if (!failed) {
                Reply("\"ok\":true,\"rows\":" + std::to_string(rows) + ",\"wall_ms\":" + wall +
                      "],\"cpu_ms\":" + cpu + "]");
            }
            continue;
        }
        if (op == "DUMP" && f.size() >= 3) {
            const cdb::QueryResult r = conn->Query(ReadFile(f[1]));
            if (!r.ok()) {
                Fail(r.error_message());
                continue;
            }
            std::ofstream out(f[2]);
            for (cdb::idx_t row = 0; row < r.RowCount(); row++) {
                for (cdb::idx_t col = 0; col < r.ColumnCount(); col++) {
                    if (col) {
                        out << '|';
                    }
                    const cdb::Value v = r.GetValue(col, row);
                    if (v.IsNull()) {
                        continue;
                    }
                    if (v.type().id() == cdb::TypeId::Double) {
                        char buf[40];
                        std::snprintf(buf, sizeof buf, "%.17g", v.GetDouble());
                        out << buf;
                    } else if (v.type().id() == cdb::TypeId::Varchar) {
                        WriteCell(out, v.GetVarchar());
                    } else {
                        WriteCell(out, v.ToString());
                    }
                }
                out << '\n';
            }
            Reply("\"ok\":true,\"rows\":" + std::to_string(r.RowCount()));
            continue;
        }
        Fail("unknown request: " + op);
    }
    return 0;
}
