#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cdb {

// How a file is opened.
enum class OpenMode : uint8_t {
    Read,      // an existing file, read only
    ReadWrite, // an existing file, read and write
    Create,    // a new, empty file; an existing one is truncated to zero bytes. Read and write.
};

// An open file. Reads and writes are positional (no shared cursor), so one handle can be read from
// several threads. Every failure is an Error(ErrorCode::Io). Writes are NOT durable until Sync().
class FileHandle {
  public:
    virtual ~FileHandle() = default;

    virtual uint64_t Size() const = 0;
    // Reads exactly `n` bytes at `offset`; an Error if the file ends first.
    virtual void ReadAt(uint64_t offset, void* dst, size_t n) const = 0;
    // Writes `n` bytes at `offset`, extending the file (a gap reads as zeros).
    virtual void WriteAt(uint64_t offset, const void* src, size_t n) = 0;
    virtual void Truncate(uint64_t size) = 0;
    // Makes this file's contents and size durable (fsync).
    virtual void Sync() = 0;
};

// An exclusive advisory lock on a path, released when the object is destroyed.
class FileLock {
  public:
    virtual ~FileLock() = default;
};

// Everything the storage layer asks of the operating system, so the database can run against the
// real one (PosixFileSystem) or against MemoryFileSystem, which can simulate a power cut at any
// point and inject failures (io/memory_file_system.h). Paths use '/'.
//
// Durability, as on POSIX: file data is durable after FileHandle::Sync(); a *name* (creating a
// file, renaming, removing) is durable after SyncDirectory() of its directory. Rename is atomic
// (readers and a crash see the old name or the new, never both or neither) and replaces an existing
// target.
class FileSystem {
  public:
    virtual ~FileSystem() = default;

    virtual std::unique_ptr<FileHandle> Open(const std::string& path, OpenMode mode) = 0;
    virtual bool Exists(const std::string& path) = 0;
    // Removes a file; returns whether it existed.
    virtual bool Remove(const std::string& path) = 0;
    virtual void Rename(const std::string& from, const std::string& to) = 0;
    // `path` and any missing parents. Succeeds if the directory exists.
    virtual void CreateDirectories(const std::string& path) = 0;
    // The names (not paths) of the entries of a directory, sorted. An Error if it does not exist.
    virtual std::vector<std::string> List(const std::string& dir) = 0;
    virtual void SyncDirectory(const std::string& dir) = 0;
    // Takes an exclusive lock, or throws an Error(Io) if somebody else holds it.
    virtual std::unique_ptr<FileLock> Lock(const std::string& path) = 0;
};

// "a" + "b" -> "a/b" (no doubled slash).
std::string JoinPath(const std::string& dir, const std::string& name);

// The operating system's file system (one shared instance).
std::shared_ptr<FileSystem> PosixFileSystem();

} // namespace cdb
