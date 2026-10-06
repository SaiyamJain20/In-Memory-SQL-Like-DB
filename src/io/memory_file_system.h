#pragma once

#include "io/file_system.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cdb {

// What survives a simulated power cut. Data written but not fsynced, and names created / renamed /
// removed but whose directory was not fsynced, may or may not have reached the disk.
struct CrashPolicy {
    enum class Kind : uint8_t {
        DropUnsynced, // only what was synced survives
        KeepAll,      // everything survives (a process crash, the OS cache intact)
        Random,       // each file and the directory keep a random part of what was not synced
    };
    Kind kind = Kind::DropUnsynced;
    uint64_t seed = 0;
    // Random only. false: each file keeps a random *prefix* of its unsynced writes, the next one
    // possibly torn at a random byte (what appending to a log looks like after a power cut).
    // true: each unsynced write independently survives or not (holes read as zeros), the harshest
    // reordering a file system may legally do.
    bool reorder_writes = false;

    static CrashPolicy DropUnsynced() { return {Kind::DropUnsynced, 0, false}; }
    static CrashPolicy KeepAll() { return {Kind::KeepAll, 0, false}; }
    static CrashPolicy Random(uint64_t seed, bool reorder_writes = false) {
        return {Kind::Random, seed, reorder_writes};
    }
};

// A file system in memory that models durability, for tests (and the fuzz targets).
//
// Every file has the bytes a reader sees now and the bytes that are durable; writes since the last
// Sync() are remembered individually. The namespace likewise has a live and a durable version, and
// the operations since the last SyncDirectory() are remembered in order. Crash() builds the file
// system a restarted machine would find. Directories themselves are always durable. Thread-safe.
class MemoryFileSystem final : public FileSystem {
  public:
    MemoryFileSystem();
    ~MemoryFileSystem() override;

    std::unique_ptr<FileHandle> Open(const std::string& path, OpenMode mode) override;
    bool Exists(const std::string& path) override;
    bool Remove(const std::string& path) override;
    void Rename(const std::string& from, const std::string& to) override;
    void CreateDirectories(const std::string& path) override;
    std::vector<std::string> List(const std::string& dir) override;
    void SyncDirectory(const std::string& dir) override;
    std::unique_ptr<FileLock> Lock(const std::string& path) override;

    // The file system after a power cut under `policy`; this one is left as it is. Nothing is
    // locked in the result and no handle is open.
    std::shared_ptr<MemoryFileSystem> Crash(const CrashPolicy& policy) const;

    // ---- test conveniences (not part of the FileSystem interface) ------------------------
    // The current contents of a file; an Error if it does not exist.
    std::vector<uint8_t> Contents(const std::string& path) const;
    // Creates or replaces a file with durable contents (to plant corrupted files).
    void SetContents(const std::string& path, const std::vector<uint8_t>& bytes);
    // All file paths, sorted.
    std::vector<std::string> Paths() const;
    // Writes, truncations and fsyncs not yet durable: > 0 means a crash now could lose something.
    size_t UnsyncedOperations() const;

  private:
    struct Inode;
    struct DirOp;
    struct LockState;
    class Handle;

    static std::string Parent(const std::string& path);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Inode>> live_;
    std::map<std::string, std::shared_ptr<Inode>> durable_;
    std::vector<DirOp> dir_ops_;
    std::set<std::string> dirs_;
    std::shared_ptr<LockState> locks_;
};

// Wraps a file system and fails on command. Every *mutating* operation (opening for writing, a
// write, a truncate, an fsync, a rename, a remove, a directory fsync, creating a directory) is
// numbered from 0 as it is attempted.
//   CrashAtOperation(n): operation n does not happen, and neither does anything after it: every
//     later call, reads included, throws, as if the process had died. Take the survivors from the
//     underlying MemoryFileSystem::Crash().
//   FailOperation(n): only operation n throws, and the system carries on (a full disk, a bad
//     sector, a failed fsync).
// Both throw Error(ErrorCode::Io).
class FaultInjector final : public FileSystem {
  public:
    static constexpr uint64_t kNever = ~uint64_t{0};

    explicit FaultInjector(std::shared_ptr<FileSystem> inner);

    void CrashAtOperation(uint64_t index) { crash_at_ = index; }
    void FailOperation(uint64_t index) { fail_at_ = index; }
    uint64_t operations() const { return operations_.load(); }
    bool crashed() const { return crashed_.load(); }
    // Crashed state is sticky; this also clears the counters and faults (a restart of the
    // "machine").
    void Reset();

    std::unique_ptr<FileHandle> Open(const std::string& path, OpenMode mode) override;
    bool Exists(const std::string& path) override;
    bool Remove(const std::string& path) override;
    void Rename(const std::string& from, const std::string& to) override;
    void CreateDirectories(const std::string& path) override;
    std::vector<std::string> List(const std::string& dir) override;
    void SyncDirectory(const std::string& dir) override;
    std::unique_ptr<FileLock> Lock(const std::string& path) override;

  private:
    class Handle;
    void Mutating(const char* what);
    void Reading(const char* what);

    std::shared_ptr<FileSystem> inner_;
    std::atomic<uint64_t> operations_{0};
    std::atomic<uint64_t> crash_at_{kNever};
    std::atomic<uint64_t> fail_at_{kNever};
    std::atomic<bool> crashed_{false};
};

} // namespace cdb
