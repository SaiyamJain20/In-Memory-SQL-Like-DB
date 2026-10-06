#include "storage/checkpoint.h"

#include "storage/binary_io.h"
#include "storage/checksum.h"
#include "storage/segment_io.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace cdb {

namespace {

constexpr char kHeaderMagic[8] = {'C', 'D', 'B', 'C', 'K', 'P', 'T', '1'};
constexpr char kTrailerMagic[8] = {'C', 'D', 'B', 'E', 'N', 'D', '0', '1'};
constexpr size_t kHeaderSize = 32;
constexpr size_t kTrailerSize = 24;
constexpr size_t kBlockOverhead = 12; // kind, padding, length, crc
constexpr uint8_t kSegmentBlock = 1;
constexpr uint8_t kFooterBlock = 2;
constexpr size_t kMaxColumns = 4096;
constexpr uint64_t kMaxRowGroupSize = idx_t{1} << 26;

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// [kind][0 0 0][length][payload][crc over everything before it]
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

// Verifies a whole block read from the file and returns its payload (a view into `block`).
std::pair<const uint8_t*, size_t> Unframe(const std::vector<uint8_t>& block, uint8_t expected_kind,
                                          const std::string& what) {
    BinaryReader r(block.data(), block.size(), what);
    if (block.size() < kBlockOverhead) {
        r.Fail("a block of " + std::to_string(block.size()) + " bytes");
    }
    const uint8_t kind = r.U8();
    if (kind != expected_kind) {
        r.Fail("a block of kind " + std::to_string(kind) + ", expected " +
               std::to_string(expected_kind));
    }
    if (r.U8() != 0 || r.U8() != 0 || r.U8() != 0) {
        r.Fail("nonzero block padding");
    }
    const uint32_t length = r.U32();
    if (length != block.size() - kBlockOverhead) {
        r.Fail("a block that says " + std::to_string(length) + " payload bytes but holds " +
               std::to_string(block.size() - kBlockOverhead));
    }
    uint32_t stored;
    std::memcpy(&stored, block.data() + block.size() - 4, 4);
    if (Crc32c(block.data(), block.size() - 4) != stored) {
        r.Fail("checksum mismatch");
    }
    return {block.data() + 8, length};
}

struct SegmentLocation {
    uint64_t offset = 0;
    uint32_t size = 0;
};
struct GroupLocation {
    uint64_t rows = 0;
    std::vector<SegmentLocation> columns;
};

} // namespace

void WriteCheckpoint(FileSystem& fs, const std::string& path, const CheckpointImage& image,
                     TaskScheduler* scheduler) {
    auto file = fs.Open(path, OpenMode::Create);
    uint64_t offset = 0;
    const auto put = [&](const std::vector<uint8_t>& bytes) {
        if (!bytes.empty()) {
            file->WriteAt(offset, bytes.data(), bytes.size());
        }
        offset += bytes.size();
    };

    {
        BinaryWriter h;
        h.Bytes(kHeaderMagic, sizeof(kHeaderMagic));
        h.U32(kCheckpointVersion);
        h.U32(0); // flags
        h.U64(image.epoch);
        h.U32(Crc32c(h.buffer().data(), h.size()));
        h.U32(0);
        put(h.buffer());
    }

    // Row groups in batches: each batch is serialized in parallel, then written in order.
    std::vector<std::vector<GroupLocation>> locations(image.tables.size());
    const size_t batch = std::max<size_t>(1, scheduler != nullptr ? scheduler->threads() * 2 : 1);
    for (size_t t = 0; t < image.tables.size(); t++) {
        const TableImage& table = image.tables[t];
        std::vector<const RowGroup*> groups;
        for (const auto& g : table.groups) {
            if (g->count() > 0) {
                groups.push_back(g.get());
            }
        }
        for (size_t first = 0; first < groups.size(); first += batch) {
            const size_t n = std::min(batch, groups.size() - first);
            std::vector<std::vector<std::vector<uint8_t>>> blocks(n);
            const auto serialize = [&](size_t i) {
                const RowGroup& g = *groups[first + i];
                for (idx_t c = 0; c < g.ColumnCount(); c++) {
                    BinaryWriter w;
                    WriteSegment(w, g.column(c));
                    blocks[i].push_back(Frame(kSegmentBlock, w.buffer()));
                }
            };
            if (scheduler != nullptr && n > 1) {
                scheduler->ParallelFor(n, serialize);
            } else {
                for (size_t i = 0; i < n; i++) {
                    serialize(i);
                }
            }
            for (size_t i = 0; i < n; i++) {
                GroupLocation loc;
                loc.rows = groups[first + i]->count();
                for (const std::vector<uint8_t>& block : blocks[i]) {
                    loc.columns.push_back({offset, static_cast<uint32_t>(block.size())});
                    put(block);
                }
                locations[t].push_back(std::move(loc));
            }
        }
    }

    // footer: the catalog and where everything is
    BinaryWriter f;
    f.U64(image.epoch);
    f.U32(static_cast<uint32_t>(image.tables.size()));
    for (size_t t = 0; t < image.tables.size(); t++) {
        const TableImage& table = image.tables[t];
        f.String(table.name);
        f.U32(static_cast<uint32_t>(table.schema.size()));
        for (const ColumnDefinition& c : table.schema) {
            f.String(c.name);
            f.U8(static_cast<uint8_t>(c.type.id()));
            f.U8(c.not_null ? 1 : 0);
        }
        f.U64(table.row_group_size);
        f.U32(static_cast<uint32_t>(locations[t].size()));
        for (const GroupLocation& g : locations[t]) {
            f.U64(g.rows);
            for (const SegmentLocation& s : g.columns) {
                f.U64(s.offset);
                f.U32(s.size);
            }
        }
    }
    const uint64_t footer_offset = offset;
    const std::vector<uint8_t> footer = Frame(kFooterBlock, f.buffer());
    put(footer);

    BinaryWriter tr;
    tr.U64(footer_offset);
    tr.U32(static_cast<uint32_t>(footer.size()));
    tr.U32(Crc32c(tr.buffer().data(), tr.size()));
    tr.Bytes(kTrailerMagic, sizeof(kTrailerMagic));
    put(tr.buffer());
    file->Sync();
}

CheckpointImage ReadCheckpoint(FileSystem& fs, const std::string& path, TaskScheduler* scheduler) {
    const std::string name = "checkpoint '" + path + "'";
    auto file = fs.Open(path, OpenMode::Read);
    const uint64_t size = file->Size();
    if (size < kHeaderSize + kTrailerSize) {
        throw Error(ErrorCode::Corruption, name + ": the file is only " + std::to_string(size) +
                                               " bytes (an unfinished write?)");
    }

    // header
    std::vector<uint8_t> header(kHeaderSize);
    file->ReadAt(0, header.data(), header.size());
    BinaryReader h(header.data(), header.size(), name + " header");
    if (std::memcmp(h.Bytes(8), kHeaderMagic, 8) != 0) {
        h.Fail("not a cdb checkpoint file (bad magic)");
    }
    const uint32_t version = h.U32();
    if (version != kCheckpointVersion) {
        h.Fail("format version " + std::to_string(version) + ", this build reads " +
               std::to_string(kCheckpointVersion));
    }
    h.U32(); // flags
    const uint64_t epoch = h.U64();
    if (h.U32() != Crc32c(header.data(), 24)) {
        h.Fail("header checksum mismatch");
    }
    if (h.U32() != 0) {
        h.Fail("nonzero header padding");
    }

    // trailer
    std::vector<uint8_t> trailer(kTrailerSize);
    file->ReadAt(size - kTrailerSize, trailer.data(), trailer.size());
    BinaryReader t(trailer.data(), trailer.size(), name + " trailer");
    const uint64_t footer_offset = t.U64();
    const uint32_t footer_size = t.U32();
    if (t.U32() != Crc32c(trailer.data(), 12)) {
        t.Fail("trailer checksum mismatch");
    }
    if (std::memcmp(t.Bytes(8), kTrailerMagic, 8) != 0) {
        t.Fail("bad trailer magic (the file was not completely written)");
    }
    if (footer_offset < kHeaderSize || footer_size < kBlockOverhead ||
        footer_offset + footer_size + kTrailerSize != size) {
        t.Fail("the footer (offset " + std::to_string(footer_offset) + ", " +
               std::to_string(footer_size) + " bytes) does not fit a file of " +
               std::to_string(size) + " bytes");
    }

    // footer
    std::vector<uint8_t> footer_block(footer_size);
    file->ReadAt(footer_offset, footer_block.data(), footer_block.size());
    const auto [footer_data, footer_len] = Unframe(footer_block, kFooterBlock, name + " footer");
    BinaryReader f(footer_data, footer_len, name + " footer");
    if (f.U64() != epoch) {
        f.Fail("the footer's epoch differs from the header's");
    }
    CheckpointImage image;
    image.epoch = epoch;
    std::vector<std::vector<GroupLocation>> locations;
    std::unordered_set<std::string> seen_names;
    const uint32_t table_count = f.Count(4 + 4 + 8 + 4);
    for (uint32_t ti = 0; ti < table_count; ti++) {
        TableImage table;
        table.name = std::string(f.String());
        if (table.name.empty() || !seen_names.insert(Lower(table.name)).second) {
            f.Fail("an empty or duplicate table name '" + table.name + "'");
        }
        const uint32_t ncols = f.Count(4 + 1 + 1);
        if (ncols == 0 || ncols > kMaxColumns) {
            f.Fail("table '" + table.name + "' has " + std::to_string(ncols) + " columns");
        }
        std::unordered_set<std::string> columns_seen;
        for (uint32_t c = 0; c < ncols; c++) {
            std::string column_name(f.String());
            const uint8_t type_id = f.U8();
            const uint8_t not_null = f.U8();
            if (type_id > static_cast<uint8_t>(TypeId::Varchar) || not_null > 1 ||
                column_name.empty() || !columns_seen.insert(Lower(column_name)).second) {
                f.Fail("an invalid column definition in table '" + table.name + "'");
            }
            table.schema.push_back(ColumnDefinition{
                std::move(column_name), LogicalType(static_cast<TypeId>(type_id)), not_null != 0});
        }
        const uint64_t group_size = f.U64();
        if (group_size < kVectorSize || group_size > kMaxRowGroupSize ||
            group_size % kVectorSize != 0) {
            f.Fail("table '" + table.name + "' has row groups of " + std::to_string(group_size) +
                   " rows");
        }
        table.row_group_size = group_size;
        const uint32_t group_count = f.Count(8 + (8 + 4) * ncols);
        std::vector<GroupLocation> locs(group_count);
        for (GroupLocation& g : locs) {
            g.rows = f.U64();
            if (g.rows == 0 || g.rows > group_size) {
                f.Fail("a row group of " + std::to_string(g.rows) + " rows in table '" +
                       table.name + "'");
            }
            for (uint32_t c = 0; c < ncols; c++) {
                SegmentLocation s;
                s.offset = f.U64();
                s.size = f.U32();
                if (s.size < kBlockOverhead || s.offset < kHeaderSize ||
                    s.offset + s.size > footer_offset) {
                    f.Fail("a column segment at [" + std::to_string(s.offset) + ", +" +
                           std::to_string(s.size) + ") lies outside the data area");
                }
                g.columns.push_back(s);
            }
        }
        table.groups.resize(group_count);
        image.tables.push_back(std::move(table));
        locations.push_back(std::move(locs));
    }
    f.ExpectEnd();

    // every row group, in parallel
    std::vector<std::pair<size_t, size_t>> jobs;
    for (size_t ti = 0; ti < image.tables.size(); ti++) {
        for (size_t g = 0; g < locations[ti].size(); g++) {
            jobs.emplace_back(ti, g);
        }
    }
    const auto load = [&](size_t j) {
        const auto [ti, gi] = jobs[j];
        TableImage& table = image.tables[ti];
        const GroupLocation& loc = locations[ti][gi];
        std::vector<std::shared_ptr<ColumnSegment>> segments;
        for (size_t c = 0; c < loc.columns.size(); c++) {
            const SegmentLocation& s = loc.columns[c];
            const std::string what = name + ", table '" + table.name + "', row group " +
                                     std::to_string(gi) + ", column " + std::to_string(c);
            std::vector<uint8_t> block(s.size);
            file->ReadAt(s.offset, block.data(), block.size());
            const auto [data, len] = Unframe(block, kSegmentBlock, what);
            BinaryReader r(data, len, what);
            segments.push_back(ReadSegment(r, table.schema[c].type, loc.rows));
            r.ExpectEnd();
        }
        table.groups[gi] = std::make_shared<RowGroup>(std::move(segments));
    };
    if (scheduler != nullptr && jobs.size() > 1) {
        scheduler->ParallelFor(jobs.size(), load);
    } else {
        for (size_t j = 0; j < jobs.size(); j++) {
            load(j);
        }
    }
    return image;
}

} // namespace cdb
