#include "storage/wal.h"

#include "common/error.h"
#include "storage/binary_io.h"
#include "storage/checksum.h"

#include <cstring>

namespace cdb {

namespace {

constexpr char kWalMagic[8] = {'C', 'D', 'B', 'W', 'A', 'L', '0', '1'};

std::vector<uint8_t> HeaderBytes(uint64_t epoch) {
    BinaryWriter w;
    w.Bytes(kWalMagic, sizeof(kWalMagic));
    w.U64(epoch);
    return w.Take();
}

// CRC over length, sequence, flags and payload (the fields of the frame header except the CRC).
uint32_t FrameCrc(uint32_t length, uint64_t sequence, uint32_t flags, const uint8_t* payload) {
    uint8_t head[16];
    std::memcpy(head, &length, 4);
    std::memcpy(head + 4, &sequence, 8);
    std::memcpy(head + 12, &flags, 4);
    return Crc32c(payload, length, Crc32c(head, sizeof(head)));
}

struct FrameHeader {
    uint32_t length;
    uint32_t crc;
    uint64_t sequence;
    uint32_t flags;
};

FrameHeader ParseFrameHeader(const uint8_t* bytes) {
    FrameHeader h;
    std::memcpy(&h.length, bytes, 4);
    std::memcpy(&h.crc, bytes + 4, 4);
    std::memcpy(&h.sequence, bytes + 8, 8);
    std::memcpy(&h.flags, bytes + 16, 4);
    return h;
}

} // namespace

WalWriter::WalWriter(std::unique_ptr<FileHandle> file, std::string path, uint64_t epoch,
                     uint64_t size, uint64_t next_seq)
    : file_(std::move(file)), path_(std::move(path)), epoch_(epoch), size_(size),
      next_seq_(next_seq) {}

std::unique_ptr<WalWriter> WalWriter::Create(FileSystem& fs, const std::string& path,
                                             uint64_t epoch) {
    auto file = fs.Open(path, OpenMode::Create);
    const std::vector<uint8_t> header = HeaderBytes(epoch);
    file->WriteAt(0, header.data(), header.size());
    file->Sync();
    return std::unique_ptr<WalWriter>(
        new WalWriter(std::move(file), path, epoch, header.size(), 0));
}

std::unique_ptr<WalWriter> WalWriter::Continue(FileSystem& fs, const std::string& path,
                                               uint64_t epoch, uint64_t valid_size,
                                               uint64_t next_seq) {
    if (valid_size < kWalHeaderSize) {
        return Create(fs, path, epoch);
    }
    auto file = fs.Open(path, OpenMode::ReadWrite);
    if (file->Size() != valid_size) {
        file->Truncate(valid_size);
        file->Sync();
    }
    return std::unique_ptr<WalWriter>(
        new WalWriter(std::move(file), path, epoch, valid_size, next_seq));
}

void WalWriter::CheckUsable() const {
    if (poisoned_) {
        throw Error(ErrorCode::Io, "the write-ahead log '" + path_ +
                                       "' is unusable after an earlier I/O error; the database "
                                       "must be reopened");
    }
}

void WalWriter::AppendFrame(const uint8_t* payload, size_t size, bool commit) {
    CheckUsable();
    if (size > 0xFFFFFFF0u) {
        throw Error(ErrorCode::Execution,
                    "a write-ahead log frame cannot hold " + std::to_string(size) + " bytes");
    }
    const auto length = static_cast<uint32_t>(size);
    const uint32_t flags = commit ? kWalCommitFlag : 0;
    const uint32_t crc = FrameCrc(length, next_seq_, flags, payload);
    BinaryWriter w;
    w.U32(length);
    w.U32(crc);
    w.U64(next_seq_);
    w.U32(flags);
    w.Bytes(payload, size);
    try {
        file_->WriteAt(size_, w.buffer().data(), w.size());
    } catch (...) {
        poisoned_ = true; // a prefix of the frame may be on disk
        throw;
    }
    size_ += w.size();
    next_seq_++;
}

void WalWriter::Sync() {
    CheckUsable();
    try {
        file_->Sync();
    } catch (...) {
        poisoned_ = true; // after a failed fsync nothing about the file's contents can be trusted
        throw;
    }
}

WalScan ScanWal(FileSystem& fs, const std::string& path, uint64_t epoch) {
    WalScan scan;
    auto file = fs.Open(path, OpenMode::Read);
    scan.file_size = file->Size();
    if (scan.file_size < kWalHeaderSize) {
        return scan; // being created when the machine stopped
    }
    uint8_t header[kWalHeaderSize];
    file->ReadAt(0, header, sizeof(header));
    if (std::memcmp(header, kWalMagic, sizeof(kWalMagic)) != 0) {
        // a header torn before it was complete reads as garbage only if the file is tiny; a file of
        // real size that is not a log is not ours to discard
        if (scan.file_size == kWalHeaderSize) {
            return scan;
        }
        throw Error(ErrorCode::Corruption, "'" + path + "' is not a write-ahead log (bad magic)");
    }
    uint64_t file_epoch;
    std::memcpy(&file_epoch, header + 8, 8);
    if (file_epoch != epoch) {
        throw Error(ErrorCode::Corruption, "'" + path + "' belongs to epoch " +
                                               std::to_string(file_epoch) + ", its name says " +
                                               std::to_string(epoch));
    }
    scan.header_valid = true;
    scan.valid_size = kWalHeaderSize;

    uint64_t pos = kWalHeaderSize;
    uint64_t sequence = 0;
    uint64_t pending_frames = 0;
    std::vector<uint8_t> payload;
    while (scan.file_size - pos >= kWalFrameHeaderSize) {
        uint8_t raw[kWalFrameHeaderSize];
        file->ReadAt(pos, raw, sizeof(raw));
        const FrameHeader h = ParseFrameHeader(raw);
        if (h.length > scan.file_size - pos - kWalFrameHeaderSize || h.sequence != sequence ||
            (h.flags & ~kWalCommitFlag) != 0) {
            break;
        }
        payload.resize(h.length);
        if (h.length > 0) {
            file->ReadAt(pos + kWalFrameHeaderSize, payload.data(), h.length);
        }
        if (FrameCrc(h.length, h.sequence, h.flags, payload.data()) != h.crc) {
            break;
        }
        pos += kWalFrameHeaderSize + h.length;
        sequence++;
        pending_frames++;
        if ((h.flags & kWalCommitFlag) != 0) {
            scan.valid_size = pos;
            scan.next_sequence = sequence;
            scan.committed_frames += pending_frames;
            scan.committed_transactions++;
            pending_frames = 0;
        }
    }
    scan.discarded_frames = pending_frames;
    return scan;
}

void ReplayWal(FileSystem& fs, const std::string& path, const WalScan& scan,
               const std::function<void(const uint8_t*, size_t)>& visit) {
    if (!scan.header_valid) {
        return;
    }
    auto file = fs.Open(path, OpenMode::Read);
    uint64_t pos = kWalHeaderSize;
    std::vector<uint8_t> payload;
    while (pos < scan.valid_size) {
        uint8_t raw[kWalFrameHeaderSize];
        file->ReadAt(pos, raw, sizeof(raw));
        const FrameHeader h = ParseFrameHeader(raw);
        payload.resize(h.length);
        if (h.length > 0) {
            file->ReadAt(pos + kWalFrameHeaderSize, payload.data(), h.length);
        }
        // the scan verified these bytes, but the file could have changed since: check again
        if (pos + kWalFrameHeaderSize + h.length > scan.valid_size ||
            FrameCrc(h.length, h.sequence, h.flags, payload.data()) != h.crc) {
            throw Error(ErrorCode::Corruption,
                        "'" + path + "' changed while it was being replayed");
        }
        visit(payload.data(), payload.size());
        pos += kWalFrameHeaderSize + h.length;
    }
}

} // namespace cdb
