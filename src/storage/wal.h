#pragma once

#include "io/file_system.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cdb {

// The write-ahead log: the statements committed since the last checkpoint, as a sequence of frames.
//
//   file   : "CDBWAL01" + the epoch (u64), then frames
//   frame  : [payload length u32][CRC-32C u32][sequence u64][flags u32][payload]
//            the CRC covers the length, sequence, flags and payload; sequence numbers start at 0
//            and increase by one; flag bit 0 marks the last frame of a transaction (its commit)
//
// A transaction is one or more frames ending in a commit frame. Recovery trusts a frame only if it
// is complete, its checksum and sequence number are right, and a commit frame follows it - so a
// transaction that was being written when the power went out simply does not exist afterwards.
inline constexpr size_t kWalHeaderSize = 16;
inline constexpr size_t kWalFrameHeaderSize = 20;
inline constexpr uint32_t kWalCommitFlag = 1;

// Appends frames to one log file. Not thread-safe (the storage manager's commit mutex serialises
// writers). After any I/O failure the writer is *poisoned*: the file may now hold a torn frame, so
// nothing more is written to it, and every later call throws.
class WalWriter {
  public:
    // A new, empty log: writes the header and fsyncs the file. The caller fsyncs the directory.
    static std::unique_ptr<WalWriter> Create(FileSystem& fs, const std::string& path,
                                             uint64_t epoch);
    // Carries on with an existing log after recovery: cuts the file back to `valid_size` (the end
    // of the last committed transaction; anything after it is a torn or uncommitted tail), makes
    // that durable, and appends from there with sequence number `next_seq`. A file too short to
    // hold a header (a crash while it was being created) is started afresh.
    static std::unique_ptr<WalWriter> Continue(FileSystem& fs, const std::string& path,
                                               uint64_t epoch, uint64_t valid_size,
                                               uint64_t next_seq);

    // Appends one frame; `commit` marks the end of a transaction. Does not fsync.
    void AppendFrame(const uint8_t* payload, size_t size, bool commit);
    void AppendFrame(const std::vector<uint8_t>& payload, bool commit) {
        AppendFrame(payload.data(), payload.size(), commit);
    }
    void Sync();

    uint64_t size() const noexcept { return size_; }
    uint64_t epoch() const noexcept { return epoch_; }
    uint64_t next_sequence() const noexcept { return next_seq_; }
    bool poisoned() const noexcept { return poisoned_; }
    const std::string& path() const noexcept { return path_; }

  private:
    WalWriter(std::unique_ptr<FileHandle> file, std::string path, uint64_t epoch, uint64_t size,
              uint64_t next_seq);
    void CheckUsable() const;

    std::unique_ptr<FileHandle> file_;
    std::string path_;
    uint64_t epoch_;
    uint64_t size_;
    uint64_t next_seq_;
    bool poisoned_ = false;
};

// What reading a log from the start finds.
struct WalScan {
    // false when the file is too short for a header or does not start with the magic: the log was
    // being created when the machine stopped. It then holds nothing.
    bool header_valid = false;
    uint64_t file_size = 0;
    // The end of the last committed transaction (the header size if there is none): everything
    // before it is good, everything after it is discarded.
    uint64_t valid_size = 0;
    uint64_t next_sequence = 0; // of the frame that would follow the last committed one
    uint64_t committed_frames = 0;
    uint64_t committed_transactions = 0;
    uint64_t discarded_frames = 0; // well-formed frames of a transaction that never committed
    uint64_t discarded_bytes() const noexcept { return file_size - valid_size; }
};

// Scans the log at `path`, which must belong to `epoch` (a header from another epoch is
// corruption: Error(Corruption)). Stops at the first frame that does not verify.
WalScan ScanWal(FileSystem& fs, const std::string& path, uint64_t epoch);

// Calls visit(payload, size) for the frames of the committed transactions found by `scan`, in
// order.
void ReplayWal(FileSystem& fs, const std::string& path, const WalScan& scan,
               const std::function<void(const uint8_t*, size_t)>& visit);

} // namespace cdb
