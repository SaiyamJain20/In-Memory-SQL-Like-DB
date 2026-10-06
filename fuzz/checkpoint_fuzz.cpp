// libFuzzer harness for the checkpoint file reader. The bytes are a checkpoint file. Contract:
//   * the reader returns an image or throws Error(Corruption) - never anything else, never crashes,
//     never reads out of bounds, never allocates absurd amounts for a tiny file;
//   * every row group of an accepted image can be scanned end to end (all vectors decode);
//   * an accepted image can be written out and read back, with the same rows.

#include "io/memory_file_system.h"
#include "storage/checkpoint.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Violation(const char* what) {
    std::fprintf(stderr, "CONTRACT VIOLATION: %s\n", what);
    std::abort();
}

// Decodes every vector of every segment (what a scan would do) and returns a cheap digest.
uint64_t ScanEverything(const cdb::CheckpointImage& image) {
    uint64_t digest = image.epoch;
    for (const cdb::TableImage& table : image.tables) {
        for (const auto& group : table.groups) {
            for (cdb::idx_t c = 0; c < group->ColumnCount(); c++) {
                const cdb::ColumnSegment& seg = group->column(c);
                cdb::Vector v(seg.type(), cdb::kVectorSize);
                for (cdb::idx_t at = 0; at < seg.count(); at += cdb::kVectorSize) {
                    const cdb::idx_t n = std::min<cdb::idx_t>(cdb::kVectorSize, seg.count() - at);
                    seg.Scan(at, n, v);
                    for (cdb::idx_t i = 0; i < n; i++) {
                        digest = digest * 1000003u + v.GetValue(i).ToString().size();
                    }
                }
            }
        }
    }
    return digest;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto fs = std::make_shared<cdb::MemoryFileSystem>();
    fs->CreateDirectories("/f");
    fs->SetContents("/f/c", std::vector<uint8_t>(data, data + size));
    cdb::CheckpointImage image;
    try {
        image = cdb::ReadCheckpoint(*fs, "/f/c", nullptr);
    } catch (const cdb::Error& e) {
        if (e.code() != cdb::ErrorCode::Corruption)
            Violation("a damaged file raised something other than Corruption");
        return 0;
    }
    const uint64_t digest = ScanEverything(image);
    cdb::WriteCheckpoint(*fs, "/f/again", image, nullptr);
    cdb::CheckpointImage back;
    try {
        back = cdb::ReadCheckpoint(*fs, "/f/again", nullptr);
    } catch (const cdb::Error&) {
        Violation("a checkpoint written from an accepted image does not read back");
    }
    if (ScanEverything(back) != digest)
        Violation("the rewritten checkpoint holds different rows");
    return 0;
}
