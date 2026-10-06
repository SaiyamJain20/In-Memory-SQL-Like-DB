// cdb_shell: a minimal SQL shell.
//
//   cdb_shell                    interactive (reads statements from stdin; a statement ends at ';')
//   cdb_shell -c "SELECT 1"      run the given SQL and exit
//   cdb_shell -f script.sql      run a script
//
// Dot commands (interactive or in scripts):
//   .tables   .schema [table]   .read FILE   .timer on|off   .maxrows N|off   .help   .quit
// The database lives in memory for the lifetime of the process.

#include "common/version.h"
#include "main/connection.h"
#include "main/database.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

namespace {

struct Settings {
    bool timer = false;
    std::optional<cdb::idx_t> max_rows =
        40; // rows printed per result (the footer says how many exist)
};

std::string ReadFile(const std::string& path, bool& ok) {
    std::ifstream in(path);
    ok = static_cast<bool>(in);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs a script, printing every statement's result. Returns false if one failed.
bool RunSql(cdb::Connection& conn, const std::string& sql, const Settings& settings) {
    bool ok = true;
    const auto start = std::chrono::steady_clock::now();
    const std::vector<cdb::QueryResult> results = conn.QueryAll(sql);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (const cdb::QueryResult& r : results) {
        if (!r.ok()) {
            std::cerr << r.error_message() << "\n";
            ok = false;
            continue;
        }
        const std::string text = r.ToString(settings.max_rows);
        if (!text.empty())
            std::cout << text << "\n";
    }
    if (settings.timer)
        std::cout << "Run Time: " << seconds * 1000 << " ms\n";
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    cdb::Database db;
    cdb::Connection conn(db);
    Settings settings;

    std::string command_sql, script;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            command_sql = argv[++i];
        } else if (arg == "-f" && i + 1 < argc) {
            bool ok;
            script = ReadFile(argv[++i], ok);
            if (!ok) {
                std::cerr << "cannot read " << argv[i] << "\n";
                return 2;
            }
        } else {
            std::cerr << "usage: cdb_shell [-c SQL | -f FILE]\n";
            return 2;
        }
    }
    int exit_code = 0;
    if (!command_sql.empty()) {
        return RunSql(conn, command_sql, settings) ? 0 : 1;
    }
    if (!script.empty()) {
        settings.max_rows = std::nullopt; // scripts print everything
        return RunSql(conn, script, settings) ? 0 : 1;
    }

    std::cout << "cdb " << cdb::kVersion
              << " - in-memory columnar SQL engine. Type .quit to exit.\n";
    std::string buffer, line;
    while (true) {
        std::cout << (buffer.empty() ? "cdb> " : "...> ") << std::flush;
        if (!std::getline(std::cin, line))
            break;
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            std::istringstream in(line);
            std::string cmd, arg;
            in >> cmd >> arg;
            if (cmd == ".quit" || cmd == ".exit")
                break;
            if (cmd == ".tables") {
                for (const auto& t : db.catalog().ListTables())
                    std::cout << t << "\n";
            } else if (cmd == ".schema") {
                for (const auto& name :
                     arg.empty() ? db.catalog().ListTables() : std::vector<std::string>{arg}) {
                    auto table = db.catalog().TryGetTable(name);
                    if (!table) {
                        std::cerr << "no such table: " << name << "\n";
                        continue;
                    }
                    std::cout << table->name() << "(";
                    for (size_t c = 0; c < table->schema().size(); c++) {
                        const auto& col = table->schema()[c];
                        std::cout << (c ? ", " : "") << col.name << " " << col.type.ToString()
                                  << (col.not_null ? " NOT NULL" : "");
                    }
                    std::cout << ")\n";
                }
            } else if (cmd == ".read") {
                bool ok;
                const std::string text = ReadFile(arg, ok);
                if (!ok)
                    std::cerr << "cannot read " << arg << "\n";
                else
                    RunSql(conn, text, settings);
            } else if (cmd == ".timer") {
                settings.timer = arg == "on";
            } else if (cmd == ".maxrows") {
                if (arg == "off") {
                    settings.max_rows = std::nullopt;
                } else {
                    settings.max_rows =
                        static_cast<cdb::idx_t>(std::max(0, std::atoi(arg.c_str())));
                }
            } else if (cmd == ".help") {
                std::cout << ".tables  .schema [table]  .read FILE  .timer on|off  .maxrows N|off  "
                             ".quit\nEXPLAIN <query> shows the optimized plan.\n";
            } else {
                std::cerr << "unknown command " << cmd << "\n";
            }
            continue;
        }
        buffer += line + "\n";
        // execute once the buffer ends with a semicolon (ignoring trailing whitespace)
        const auto last = buffer.find_last_not_of(" \t\r\n");
        if (last != std::string::npos && buffer[last] == ';') {
            RunSql(conn, buffer, settings);
            buffer.clear();
        }
    }
    return exit_code;
}
