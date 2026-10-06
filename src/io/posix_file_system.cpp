#include "common/error.h"
#include "io/file_system.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace cdb {

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

namespace {

[[noreturn]] void Fail(const std::string& what, const std::string& path, int err) {
    throw Error(ErrorCode::Io, what + " '" + path + "': " + std::strerror(err));
}

class PosixHandle final : public FileHandle {
  public:
    PosixHandle(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~PosixHandle() override { ::close(fd_); }

    uint64_t Size() const override {
        const off_t end = ::lseek(fd_, 0, SEEK_END);
        if (end < 0) {
            Fail("cannot get the size of", path_, errno);
        }
        return static_cast<uint64_t>(end);
    }

    void ReadAt(uint64_t offset, void* dst, size_t n) const override {
        auto* out = static_cast<uint8_t*>(dst);
        size_t done = 0;
        while (done < n) {
            const ssize_t r = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                Fail("read failed on", path_, errno);
            }
            if (r == 0) {
                throw Error(ErrorCode::Io, "unexpected end of file reading '" + path_ +
                                               "' at offset " + std::to_string(offset + done));
            }
            done += static_cast<size_t>(r);
        }
    }

    void WriteAt(uint64_t offset, const void* src, size_t n) override {
        const auto* in = static_cast<const uint8_t*>(src);
        size_t done = 0;
        while (done < n) {
            const ssize_t r = ::pwrite(fd_, in + done, n - done, static_cast<off_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                Fail("write failed on", path_, errno);
            }
            done += static_cast<size_t>(r);
        }
    }

    void Truncate(uint64_t size) override {
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            Fail("cannot truncate", path_, errno);
        }
    }

    void Sync() override {
        while (::fsync(fd_) != 0) {
            if (errno != EINTR) {
                Fail("fsync failed on", path_, errno);
            }
        }
    }

  private:
    int fd_;
    std::string path_;
};

class PosixLock final : public FileLock {
  public:
    explicit PosixLock(int fd) : fd_(fd) {}
    ~PosixLock() override { ::close(fd_); } // closing the descriptor releases the flock

  private:
    int fd_;
};

class Posix final : public FileSystem {
  public:
    std::unique_ptr<FileHandle> Open(const std::string& path, OpenMode mode) override {
        int flags = O_CLOEXEC;
        switch (mode) {
        case OpenMode::Read:
            flags |= O_RDONLY;
            break;
        case OpenMode::ReadWrite:
            flags |= O_RDWR;
            break;
        case OpenMode::Create:
            flags |= O_RDWR | O_CREAT | O_TRUNC;
            break;
        }
        const int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0) {
            Fail("cannot open", path, errno);
        }
        return std::make_unique<PosixHandle>(fd, path);
    }

    bool Exists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

    bool Remove(const std::string& path) override {
        if (::unlink(path.c_str()) == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        Fail("cannot remove", path, errno);
    }

    void Rename(const std::string& from, const std::string& to) override {
        if (::rename(from.c_str(), to.c_str()) != 0) {
            Fail("cannot rename", from + "' to '" + to, errno);
        }
    }

    void CreateDirectories(const std::string& path) override {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) {
            Fail("cannot create directory", path, ec.value());
        }
    }

    std::vector<std::string> List(const std::string& dir) override {
        DIR* d = ::opendir(dir.c_str());
        if (d == nullptr) {
            Fail("cannot list", dir, errno);
        }
        std::vector<std::string> names;
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name != "." && name != "..") {
                names.push_back(name);
            }
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        return names;
    }

    void SyncDirectory(const std::string& dir) override {
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            Fail("cannot open directory", dir, errno);
        }
        const int rc = ::fsync(fd);
        const int err = errno;
        ::close(fd);
        if (rc != 0) {
            Fail("fsync failed on directory", dir, err);
        }
    }

    std::unique_ptr<FileLock> Lock(const std::string& path) override {
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            Fail("cannot open lock file", path, errno);
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int err = errno;
            ::close(fd);
            if (err == EWOULDBLOCK) {
                throw Error(ErrorCode::Io,
                            "the database is in use by another connection or process "
                            "(could not lock '" +
                                path + "')");
            }
            Fail("cannot lock", path, err);
        }
        return std::make_unique<PosixLock>(fd);
    }
};

} // namespace

std::shared_ptr<FileSystem> PosixFileSystem() {
    static const std::shared_ptr<FileSystem> instance = std::make_shared<Posix>();
    return instance;
}

} // namespace cdb
