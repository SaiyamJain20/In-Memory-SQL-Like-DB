// The checkpoint file: round trips of whole table images, serial and parallel, and the promise that
// a damaged file is never mistaken for a good one - every single-bit flip and every truncation is
// detected, and a file whose checksums are right but whose structure lies is rejected too.

#include "storage/checkpoint.h"

#include "io/memory_file_system.h"
#include "storage/binary_io.h"
#include "storage/checksum.h"
#include "storage/segment_io.h"
#include "storage/table.h"
#include "storage_test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

constexpr const char* kPath = "/db/checkpoint.cdb";

std::shared_ptr<MemoryFileSystem> NewFs() {
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->CreateDirectories("/db");
    return fs;
}

// A table of random rows: `rows` rows in groups of `group_size`, the last one a still-open tail.
TableImage RandomTable(const std::string& name, idx_t rows, idx_t group_size, Rng& rng,
                       double nulls = 0.2, bool compress = true) {
    const test::ScopedCompression compression(compress);
    std::vector<ColumnDefinition> schema = test::AllTypesSchema();
    schema[1].not_null = true; // exercise the flag
    Table table(name, schema, group_size);
    test::TableModel model(schema.size());
    for (idx_t done = 0; done < rows;) {
        const idx_t n = std::min<idx_t>(1 + RandBelow(rng, kVectorSize), rows - done);
        // column 1 is NOT NULL: no NULLs there
        DataChunk chunk = test::RandomChunk(schema, rng, n, model, nulls);
        for (idx_t i = 0; i < n; i++) {
            if (chunk.GetValue(1, i).IsNull()) {
                chunk.SetValue(1, i, Value::Integer(7));
            }
        }
        table.Append(chunk);
        done += n;
    }
    TableImage image;
    image.name = name;
    image.schema = schema;
    image.row_group_size = group_size;
    const auto snap = table.Snapshot();
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        image.groups.push_back(snap->row_group_ptr(g));
    }
    return image;
}

void ExpectSameImage(const CheckpointImage& got, const CheckpointImage& want) {
    ASSERT_EQ(got.epoch, want.epoch);
    ASSERT_EQ(got.tables.size(), want.tables.size());
    for (size_t t = 0; t < want.tables.size(); t++) {
        const TableImage &a = got.tables[t], &b = want.tables[t];
        ASSERT_EQ(a.name, b.name);
        ASSERT_EQ(a.row_group_size, b.row_group_size) << a.name;
        ASSERT_EQ(a.schema.size(), b.schema.size());
        for (size_t c = 0; c < a.schema.size(); c++) {
            EXPECT_EQ(a.schema[c].name, b.schema[c].name);
            EXPECT_EQ(a.schema[c].type, b.schema[c].type);
            EXPECT_EQ(a.schema[c].not_null, b.schema[c].not_null);
        }
        ASSERT_EQ(a.groups.size(), b.groups.size()) << a.name;
        for (size_t g = 0; g < a.groups.size(); g++) {
            ASSERT_EQ(a.groups[g]->count(), b.groups[g]->count());
            for (idx_t c = 0; c < a.schema.size(); c++) {
                test::ExpectSameSegment(a.groups[g]->column(c), b.groups[g]->column(c),
                                        a.name + " group " + std::to_string(g) + " column " +
                                            std::to_string(c));
                if (::testing::Test::HasFailure()) {
                    return;
                }
            }
        }
    }
}

void ExpectCorruption(const std::function<void()>& fn, const std::string& what) {
    try {
        fn();
        ADD_FAILURE() << what << ": expected a Corruption error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption) << what << ": " << e.what();
    }
}

CheckpointImage SmallImage(Rng& rng) {
    CheckpointImage image;
    image.epoch = 17;
    image.tables.push_back(RandomTable("alpha", 5000, 3 * kVectorSize, rng));
    image.tables.push_back(RandomTable("Beta", 100, kVectorSize, rng, 0.5, false));
    return image;
}

// A file with correct checksums around whatever structure the test supplies.
std::vector<uint8_t> Frame(uint8_t kind, const std::vector<uint8_t>& payload) {
    BinaryWriter w;
    w.U8(kind);
    w.U8(0);
    w.U8(0);
    w.U8(0);
    w.U32(static_cast<uint32_t>(payload.size()));
    w.Bytes(payload.data(), payload.size());
    w.U32(Crc32c(w.buffer().data(), w.size()));
    return w.Take();
}

std::vector<uint8_t> HandBuiltFile(uint64_t epoch, const std::vector<std::vector<uint8_t>>& blocks,
                                   const std::vector<uint8_t>& footer_payload,
                                   uint32_t version = kCheckpointVersion) {
    BinaryWriter w;
    w.Bytes("CDBCKPT1", 8);
    w.U32(version);
    w.U32(0);
    w.U64(epoch);
    w.U32(Crc32c(w.buffer().data(), w.size()));
    w.U32(0);
    for (const auto& b : blocks) {
        w.Bytes(b.data(), b.size());
    }
    const uint64_t footer_offset = w.size();
    const std::vector<uint8_t> footer = Frame(2, footer_payload);
    w.Bytes(footer.data(), footer.size());
    BinaryWriter t;
    t.U64(footer_offset);
    t.U32(static_cast<uint32_t>(footer.size()));
    t.U32(Crc32c(t.buffer().data(), t.size()));
    t.Bytes("CDBEND01", 8);
    w.Bytes(t.buffer().data(), t.size());
    return w.Take();
}

void Plant(MemoryFileSystem& fs, const std::vector<uint8_t>& bytes) {
    fs.SetContents(kPath, bytes);
}

} // namespace

TEST(Checkpoint, AnEmptyDatabaseRoundTrips) {
    auto fs = NewFs();
    CheckpointImage image;
    image.epoch = 5;
    WriteCheckpoint(*fs, kPath, image, nullptr);
    const CheckpointImage back = ReadCheckpoint(*fs, kPath, nullptr);
    EXPECT_EQ(back.epoch, 5U);
    EXPECT_TRUE(back.tables.empty());
    EXPECT_EQ(fs->Contents(kPath).size(), 32U + (12 + 8 + 4) + 24U)
        << "header, footer block, trailer";
}

TEST(Checkpoint, TablesOfEveryShapeRoundTrip) {
    Rng rng(1);
    for (const bool compress : {true, false}) {
        for (const double nulls : {0.0, 0.3, 1.0}) {
            CheckpointImage image;
            image.epoch = 123456789012345ULL;
            image.tables.push_back(RandomTable("empty", 0, kVectorSize, rng, nulls, compress));
            image.tables.push_back(RandomTable("one_row", 1, kVectorSize, rng, nulls, compress));
            image.tables.push_back(
                RandomTable("tail_only", 2500, 4 * kVectorSize, rng, nulls, compress));
            image.tables.push_back(
                RandomTable("exact", 3 * kVectorSize * 2, 3 * kVectorSize, rng, nulls, compress));
            image.tables.push_back(
                RandomTable("many_groups", 20000, kVectorSize, rng, nulls, compress));
            image.tables.push_back(
                RandomTable("Mixed Case", 9000, 2 * kVectorSize, rng, nulls, compress));
            auto fs = NewFs();
            WriteCheckpoint(*fs, kPath, image, nullptr);
            const CheckpointImage back = ReadCheckpoint(*fs, kPath, nullptr);
            ExpectSameImage(back, image);
            if (::testing::Test::HasFailure()) {
                return;
            }
            // reading does not change the file, and writing the same image again gives the same
            // bytes
            const std::vector<uint8_t> first = fs->Contents(kPath);
            auto again = NewFs();
            WriteCheckpoint(*again, kPath, back, nullptr);
            EXPECT_EQ(again->Contents(kPath), first) << "the format is deterministic";
        }
    }
}

TEST(Checkpoint, ParallelWriteAndReadGiveTheSameFileAndTheSameImage) {
    Rng rng(2);
    CheckpointImage image;
    image.epoch = 9;
    for (int t = 0; t < 4; t++) {
        image.tables.push_back(RandomTable("t" + std::to_string(t), 30000, 2 * kVectorSize, rng));
    }
    auto serial_fs = NewFs();
    WriteCheckpoint(*serial_fs, kPath, image, nullptr);
    for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}}) {
        TaskScheduler pool(threads);
        auto fs = NewFs();
        WriteCheckpoint(*fs, kPath, image, &pool);
        EXPECT_EQ(fs->Contents(kPath), serial_fs->Contents(kPath)) << threads << " threads";
        ExpectSameImage(ReadCheckpoint(*fs, kPath, &pool), image);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(Checkpoint, WritingSyncsTheFileButNotTheDirectory) {
    Rng rng(3);
    auto fs = NewFs();
    WriteCheckpoint(*fs, kPath, SmallImage(rng), nullptr);
    // after a crash that drops everything unsynced the file still has all its bytes, but its name
    // is not durable until the caller syncs the directory
    const auto crashed = fs->Crash(CrashPolicy::DropUnsynced());
    EXPECT_FALSE(crashed->Exists(kPath));
    fs->SyncDirectory("/db");
    const auto after = fs->Crash(CrashPolicy::DropUnsynced());
    ASSERT_TRUE(after->Exists(kPath));
    EXPECT_EQ(after->Contents(kPath), fs->Contents(kPath));
}

TEST(Checkpoint, EverySingleBitFlipIsDetected) {
    Rng rng(4);
    CheckpointImage image;
    image.epoch = 3;
    image.tables.push_back(RandomTable("t", 150, kVectorSize, rng, 0.2));
    image.tables.push_back(RandomTable("u", 12, kVectorSize, rng, 0.0, false));
    auto fs = NewFs();
    WriteCheckpoint(*fs, kPath, image, nullptr);
    const std::vector<uint8_t> good = fs->Contents(kPath);
    ASSERT_GT(good.size(), 100U);
    ASSERT_LT(good.size(), 40000U) << "keep the sweep affordable";
    ExpectSameImage(ReadCheckpoint(*fs, kPath, nullptr), image);
    for (size_t byte = 0; byte < good.size(); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            std::vector<uint8_t> damaged = good;
            damaged[byte] ^= static_cast<uint8_t>(1 << bit);
            Plant(*fs, damaged);
            try {
                ReadCheckpoint(*fs, kPath, nullptr);
                FAIL() << "flipping bit " << bit << " of byte " << byte << " (of " << good.size()
                       << ") was not detected";
            } catch (const Error& e) {
                ASSERT_EQ(e.code(), ErrorCode::Corruption)
                    << "byte " << byte << " bit " << bit << ": " << e.what();
            }
        }
    }
}

TEST(Checkpoint, EveryTruncationAndAnyAppendedByteIsDetected) {
    Rng rng(5);
    CheckpointImage image;
    image.epoch = 4;
    image.tables.push_back(RandomTable("t", 900, kVectorSize, rng));
    auto fs = NewFs();
    WriteCheckpoint(*fs, kPath, image, nullptr);
    const std::vector<uint8_t> good = fs->Contents(kPath);
    for (size_t cut = 0; cut < good.size(); cut++) {
        Plant(*fs, std::vector<uint8_t>(good.begin(), good.begin() + static_cast<long>(cut)));
        ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); },
                         "cut at " + std::to_string(cut));
    }
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    Plant(*fs, longer);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "a trailing byte");
    std::vector<uint8_t> prefixed = good;
    prefixed.insert(prefixed.begin(), 0);
    Plant(*fs, prefixed);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "a leading byte");
}

TEST(Checkpoint, AMissingFileIsAnIoErrorNotCorruption) {
    auto fs = NewFs();
    try {
        ReadCheckpoint(*fs, "/db/nothing.cdb", nullptr);
        FAIL() << "expected an error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Io);
    }
}

TEST(Checkpoint, ADifferentFormatVersionIsRefusedWithAMessage) {
    auto fs = NewFs();
    BinaryWriter footer;
    footer.U64(1);
    footer.U32(0);
    Plant(*fs, HandBuiltFile(1, {}, footer.buffer(), kCheckpointVersion + 1));
    try {
        ReadCheckpoint(*fs, kPath, nullptr);
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption);
        EXPECT_NE(std::string(e.what()).find("version"), std::string::npos) << e.what();
    }
    // and a file that is not a checkpoint at all
    Plant(*fs, std::vector<uint8_t>(200, 'x'));
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "garbage");
}

// ---- files whose checksums are right but whose structure is not --------------------------

namespace {
// footer payload for one table "t" with one INTEGER column and the given group directory
std::vector<uint8_t> Footer(uint64_t epoch, const std::function<void(BinaryWriter&)>& body) {
    BinaryWriter f;
    f.U64(epoch);
    body(f);
    return f.Take();
}
void TableHeader(BinaryWriter& f, const std::string& name, uint32_t columns, uint64_t group_size,
                 uint32_t groups) {
    f.String(name);
    f.U32(columns);
    for (uint32_t c = 0; c < columns; c++) {
        f.String("c" + std::to_string(c));
        f.U8(static_cast<uint8_t>(TypeId::Integer));
        f.U8(0);
    }
    f.U64(group_size);
    f.U32(groups);
}
} // namespace

TEST(Checkpoint, ACorrectlyChecksummedButLyingDirectoryIsRejected) {
    auto fs = NewFs();
    const auto expect_rejected = [&](const std::string& what, const std::vector<uint8_t>& footer,
                                     uint64_t epoch = 1) {
        Plant(*fs, HandBuiltFile(epoch, {}, footer));
        ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, what);
    };
    // sanity: an empty directory is fine
    Plant(*fs, HandBuiltFile(1, {}, Footer(1, [](BinaryWriter& f) { f.U32(0); })));
    EXPECT_NO_THROW(ReadCheckpoint(*fs, kPath, nullptr));

    expect_rejected("the footer's epoch differs from the header's",
                    Footer(2, [](BinaryWriter& f) { f.U32(0); }));
    expect_rejected("more tables than fit", Footer(1, [](BinaryWriter& f) { f.U32(0xFFFFFFF0u); }));
    expect_rejected("an empty table name", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "", 1, kVectorSize, 0);
                    }));
    expect_rejected("a table without columns", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 0, kVectorSize, 0);
                    }));
    expect_rejected("a row group size that is not a multiple of the vector size",
                    Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize + 1, 0);
                    }));
    expect_rejected("a zero row group size", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, 0, 0);
                    }));
    expect_rejected("duplicate table names (case-insensitively)", Footer(1, [](BinaryWriter& f) {
                        f.U32(2);
                        TableHeader(f, "t", 1, kVectorSize, 0);
                        TableHeader(f, "T", 1, kVectorSize, 0);
                    }));
    expect_rejected("duplicate column names", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        f.String("t");
                        f.U32(2);
                        for (int i = 0; i < 2; i++) {
                            f.String("same");
                            f.U8(static_cast<uint8_t>(TypeId::Integer));
                            f.U8(0);
                        }
                        f.U64(kVectorSize);
                        f.U32(0);
                    }));
    expect_rejected("an unknown column type", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        f.String("t");
                        f.U32(1);
                        f.String("c");
                        f.U8(99);
                        f.U8(0);
                        f.U64(kVectorSize);
                        f.U32(0);
                    }));
    expect_rejected("a group count that cannot fit", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize, 0xFFFFFF00u);
                    }));
    expect_rejected("a row group of zero rows", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize, 1);
                        f.U64(0);
                        f.U64(32);
                        f.U32(100);
                    }));
    expect_rejected("a row group larger than the table's", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize, 1);
                        f.U64(kVectorSize + 1);
                        f.U64(32);
                        f.U32(100);
                    }));
    expect_rejected("a segment outside the data area", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize, 1);
                        f.U64(10);
                        f.U64(1000000);
                        f.U32(100);
                    }));
    expect_rejected("a segment overlapping the header", Footer(1, [](BinaryWriter& f) {
                        f.U32(1);
                        TableHeader(f, "t", 1, kVectorSize, 1);
                        f.U64(10);
                        f.U64(0);
                        f.U32(100);
                    }));
    expect_rejected("trailing bytes in the footer", Footer(1, [](BinaryWriter& f) {
                        f.U32(0);
                        f.U8(1);
                    }));

    // a segment block of the wrong kind, a segment whose type or row count contradicts its
    // directory entry, and a block whose checksum is right but whose payload is not a segment
    const auto build_with_block = [&](const std::vector<uint8_t>& block, uint64_t rows,
                                      TypeId directory_type) {
        BinaryWriter f;
        f.U64(1);
        f.U32(1);
        f.String("t");
        f.U32(1);
        f.String("c");
        f.U8(static_cast<uint8_t>(directory_type));
        f.U8(0);
        f.U64(kVectorSize);
        f.U32(1);
        f.U64(rows);
        f.U64(32);
        f.U32(static_cast<uint32_t>(block.size()));
        Plant(*fs, HandBuiltFile(1, {block}, f.buffer()));
    };
    Rng rng(6);
    TableImage one = RandomTable("x", 10, kVectorSize, rng);
    BinaryWriter seg;
    WriteSegment(seg, one.groups[0]->column(1)); // an INTEGER column of 10 rows
    const std::vector<uint8_t> good_block = Frame(1, seg.buffer());
    build_with_block(good_block, 10, TypeId::Integer);
    EXPECT_NO_THROW(ReadCheckpoint(*fs, kPath, nullptr)) << "the hand-built file itself is valid";
    build_with_block(Frame(2, seg.buffer()), 10, TypeId::Integer);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); },
                     "a segment block of the footer's kind");
    build_with_block(good_block, 11, TypeId::Integer);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "a row count that disagrees");
    build_with_block(good_block, 10, TypeId::BigInt);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "a type that disagrees");
    build_with_block(Frame(1, {1, 2, 3}), 10, TypeId::Integer);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); },
                     "a payload that is not a segment");
    std::vector<uint8_t> trailing = seg.buffer();
    trailing.push_back(0);
    build_with_block(Frame(1, trailing), 10, TypeId::Integer);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "bytes after the segment");
}

TEST(Checkpoint, ErrorsInOneRowGroupSurfaceFromTheParallelReader) {
    Rng rng(7);
    CheckpointImage image;
    image.epoch = 2;
    image.tables.push_back(RandomTable("t", 40000, kVectorSize, rng));
    auto fs = NewFs();
    WriteCheckpoint(*fs, kPath, image, nullptr);
    std::vector<uint8_t> bytes = fs->Contents(kPath);
    bytes[bytes.size() / 2] ^= 0x40;
    Plant(*fs, bytes);
    TaskScheduler pool(4);
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, &pool); }, "damage in the middle");
    ExpectCorruption([&] { ReadCheckpoint(*fs, kPath, nullptr); }, "damage in the middle, serial");
}

} // namespace cdb
