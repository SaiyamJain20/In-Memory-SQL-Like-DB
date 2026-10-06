#pragma once

#include "execution/task_scheduler.h"
#include "io/file_system.h"
#include "storage/column_definition.h"
#include "storage/row_group.h"

#include <memory>
#include <string>
#include <vector>

namespace cdb {

// Everything a checkpoint stores about one table: its definition and its row groups, each column
// segment in the encoding it has in memory.
struct TableImage {
    std::string name;
    std::vector<ColumnDefinition> schema;
    idx_t row_group_size = kRowGroupSize;
    std::vector<std::shared_ptr<const RowGroup>> groups;
};

// A snapshot of a whole database at one instant: what a checkpoint file holds.
struct CheckpointImage {
    uint64_t epoch = 0;
    std::vector<TableImage> tables;
};

// A checkpoint file is written once and never modified (ADR 0009):
//
//   header   32 bytes   "CDBCKPT1", format version, flags, epoch, CRC-32C of the first 24 bytes
//   blocks              one per column segment: [kind u8][3 zero bytes][payload length u32]
//                       [payload][CRC-32C of everything before it in the block]
//   footer              one block: the catalog and the directory of every segment's position
//   trailer  24 bytes   footer offset, footer block size, CRC-32C of those two, "CDBEND01"
//
// A reader starts from the trailer, so a file that was cut short (an unfinished write) is
// recognised at once, and every byte it uses is covered by a checksum.
inline constexpr uint32_t kCheckpointVersion = 1;

// Creates (or truncates) `path`, writes the image and fsyncs the file. Does not touch the
// directory: the caller renames the file into place and fsyncs the directory. Column segments are
// serialized on the scheduler's threads (null: the caller's) and written in order.
void WriteCheckpoint(FileSystem& fs, const std::string& path, const CheckpointImage& image,
                     TaskScheduler* scheduler);

// Reads and verifies a checkpoint file; row groups are decoded in parallel on the scheduler's
// threads (null: the caller's). Throws Error(ErrorCode::Corruption) if anything fails to verify (a
// truncated file, a checksum mismatch, a structure that does not hold together), Error(Io) if
// the file cannot be read.
CheckpointImage ReadCheckpoint(FileSystem& fs, const std::string& path, TaskScheduler* scheduler);

} // namespace cdb
