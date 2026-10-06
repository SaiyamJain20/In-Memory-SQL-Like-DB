// libFuzzer harness for write-ahead-log recovery. The first byte selects how the file is framed,
// the rest are its bytes; the database in the directory is opened (recovery). Contract:
//   * opening succeeds or throws Error(Corruption) - never anything else, never crashes;
//   * whatever recovered can be queried, checkpointed and recovered again with identical contents
//     (recovery of a checkpoint of a recovered log equals recovery of the log).

#include "io/memory_file_system.h"
#include "main/connection.h"
#include "main/database.h"
#include "storage/wal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Violation(const char* what) {
    std::fprintf(stderr, "CONTRACT VIOLATION: %s\n", what);
    std::abort();
}

std::string Dump(cdb::Database& db) {
    std::string out;
    cdb::Connection conn(db);
    for (const std::string& name : db.catalog().ListTables()) {
        const auto table = db.catalog().GetTable(name);
        out += "T " + table->name();
        for (const auto& c : table->schema())
            out += " " + c.name + ":" + c.type.ToString() + (c.not_null ? "!" : "");
        out += "\n";
        const cdb::QueryResult r = conn.Query("SELECT * FROM \"" + name + "\"");
        if (!r.ok())
            Violation("a recovered table cannot be read");
        out += r.ToString() + "\n";
    }
    return out;
}

cdb::DatabaseOptions Options(const std::shared_ptr<cdb::FileSystem>& fs) {
    cdb::DatabaseOptions o;
    o.threads = 1;
    o.storage.fs = fs;
    o.storage.checkpoint_on_close = false;
    return o;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;
    const uint8_t mode = data[0];
    auto fs = std::make_shared<cdb::MemoryFileSystem>();
    fs->CreateDirectories("/db");
    if ((mode & 2) != 0) {
        // The rest of the input is the payload of one committed frame, framed (and checksummed)
        // here, so the fuzzer's bytes reach the operation decoder instead of bouncing off the CRC.
        auto wal = cdb::WalWriter::Create(*fs, "/db/wal-0000000000000000.log", 0);
        wal->AppendFrame(data + 1, size - 1, true);
        wal->Sync();
    } else {
        std::vector<uint8_t> file;
        if ((mode & 1) != 0) { // a good header for epoch 0, then the fuzzer's frames
            const char magic[8] = {'C', 'D', 'B', 'W', 'A', 'L', '0', '1'};
            file.insert(file.end(), magic, magic + 8);
            file.insert(file.end(), 8, 0);
        }
        file.insert(file.end(), data + 1, data + size);
        fs->SetContents("/db/wal-0000000000000000.log", file);
    }
    std::string first;
    try {
        cdb::Database db("/db", Options(fs));
        first = Dump(db);
        db.Checkpoint();
    } catch (const cdb::Error& e) {
        if (e.code() != cdb::ErrorCode::Corruption)
            Violation("a damaged log raised something other than Corruption");
        return 0;
    }
    try {
        cdb::Database again("/db", Options(fs));
        if (Dump(again) != first)
            Violation("recovering from the checkpoint differs from recovering from the log");
    } catch (const cdb::Error&) {
        Violation("a database that was just checkpointed does not reopen");
    }
    return 0;
}
