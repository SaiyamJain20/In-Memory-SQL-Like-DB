#include "io/memory_file_system.h"

#include "common/error.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace cdb {

namespace {

// SplitMix64: a small generator whose sequence is the same on every platform, so a crash seed
// reproduces everywhere.
class Rng {
  public:
    explicit Rng(uint64_t seed) : state_(seed) {}
    uint64_t Next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // 0..n inclusive
    size_t UpTo(size_t n) { return static_cast<size_t>(Next() % (static_cast<uint64_t>(n) + 1)); }
    bool Coin() { return (Next() & 1) != 0; }

  private:
    uint64_t state_;
};

[[noreturn]] void NoSuchFile(const std::string& path) {
    throw Error(ErrorCode::Io, "no such file or directory: '" + path + "'");
}

std::string Trim(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    return dir;
}

} // namespace

struct MemoryFileSystem::Inode {
    struct Op {
        bool truncate = false;
        uint64_t at = 0; // write offset, or the new size of a truncate
        std::vector<uint8_t> bytes;
    };
    std::vector<uint8_t> data;    // what a reader sees now
    std::vector<uint8_t> durable; // what survives a crash if no pending operation does
    std::vector<Op> pending;      // since the last Sync(), in order
};

struct MemoryFileSystem::DirOp {
    enum class Kind : uint8_t { Link, Rename, Unlink };
    Kind kind;
    std::string dir; // directory the operation belongs to (SyncDirectory makes it durable)
    std::string a, b;
    std::shared_ptr<Inode> inode;
};

struct MemoryFileSystem::LockState {
    std::mutex mutex;
    std::set<std::string> held;
};

namespace {
void ApplyOp(std::vector<uint8_t>& bytes, bool truncate, uint64_t at, const uint8_t* data,
             size_t n) {
    if (truncate) {
        bytes.resize(at, 0);
        return;
    }
    if (bytes.size() < at + n) {
        bytes.resize(at + n, 0);
    }
    if (n > 0) {
        std::memcpy(bytes.data() + at, data, n);
    }
}
} // namespace

class MemoryFileSystem::Handle final : public FileHandle {
  public:
    Handle(MemoryFileSystem& fs, std::shared_ptr<Inode> inode, std::string path, bool writable)
        : fs_(fs), inode_(std::move(inode)), path_(std::move(path)), writable_(writable) {}

    uint64_t Size() const override {
        const std::lock_guard<std::mutex> lock(fs_.mutex_);
        return inode_->data.size();
    }
    void ReadAt(uint64_t offset, void* dst, size_t n) const override {
        const std::lock_guard<std::mutex> lock(fs_.mutex_);
        if (offset + n > inode_->data.size()) {
            throw Error(ErrorCode::Io, "unexpected end of file reading '" + path_ + "' at offset " +
                                           std::to_string(offset));
        }
        if (n > 0) {
            std::memcpy(dst, inode_->data.data() + offset, n);
        }
    }
    void WriteAt(uint64_t offset, const void* src, size_t n) override {
        CheckWritable();
        const std::lock_guard<std::mutex> lock(fs_.mutex_);
        const auto* in = static_cast<const uint8_t*>(src);
        ApplyOp(inode_->data, false, offset, in, n);
        inode_->pending.push_back({false, offset, std::vector<uint8_t>(in, in + n)});
    }
    void Truncate(uint64_t size) override {
        CheckWritable();
        const std::lock_guard<std::mutex> lock(fs_.mutex_);
        ApplyOp(inode_->data, true, size, nullptr, 0);
        inode_->pending.push_back({true, size, {}});
    }
    void Sync() override {
        CheckWritable();
        const std::lock_guard<std::mutex> lock(fs_.mutex_);
        inode_->durable = inode_->data;
        inode_->pending.clear();
    }

  private:
    void CheckWritable() const {
        if (!writable_) {
            throw Error(ErrorCode::Io, "'" + path_ + "' is open read-only");
        }
    }
    MemoryFileSystem& fs_;
    std::shared_ptr<Inode> inode_;
    std::string path_;
    bool writable_;
};

MemoryFileSystem::MemoryFileSystem() : locks_(std::make_shared<LockState>()) {
    dirs_.insert("");
    dirs_.insert("/");
}
MemoryFileSystem::~MemoryFileSystem() = default;

std::string MemoryFileSystem::Parent(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return "";
    }
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::unique_ptr<FileHandle> MemoryFileSystem::Open(const std::string& path, OpenMode mode) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = live_.find(path);
    if (mode == OpenMode::Create) {
        if (it == live_.end()) {
            if (dirs_.count(Parent(path)) == 0) {
                NoSuchFile(Parent(path));
            }
            auto inode = std::make_shared<Inode>();
            live_[path] = inode;
            dir_ops_.push_back({DirOp::Kind::Link, Parent(path), path, "", inode});
            return std::make_unique<Handle>(*this, inode, path, true);
        }
        it->second->data
            .clear(); // truncating an existing file is an unsynced change of its contents
        it->second->pending.push_back({true, 0, {}});
        return std::make_unique<Handle>(*this, it->second, path, true);
    }
    if (it == live_.end()) {
        NoSuchFile(path);
    }
    return std::make_unique<Handle>(*this, it->second, path, mode == OpenMode::ReadWrite);
}

bool MemoryFileSystem::Exists(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return live_.count(path) != 0 || dirs_.count(Trim(path)) != 0;
}

bool MemoryFileSystem::Remove(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(path);
    if (it == live_.end()) {
        return false;
    }
    dir_ops_.push_back({DirOp::Kind::Unlink, Parent(path), path, "", it->second});
    live_.erase(it);
    return true;
}

void MemoryFileSystem::Rename(const std::string& from, const std::string& to) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(from);
    if (it == live_.end()) {
        NoSuchFile(from);
    }
    if (dirs_.count(Parent(to)) == 0) {
        NoSuchFile(Parent(to));
    }
    std::shared_ptr<Inode> inode = it->second;
    live_.erase(it);
    live_[to] = inode;
    // a rename across directories would need both directories synced; the engine never does one
    dir_ops_.push_back({DirOp::Kind::Rename, Parent(to), from, to, inode});
}

void MemoryFileSystem::CreateDirectories(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string p = Trim(path);
    while (!p.empty() && p != "/") {
        dirs_.insert(p);
        p = Parent(p);
    }
}

std::vector<std::string> MemoryFileSystem::List(const std::string& dir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string d = Trim(dir);
    if (dirs_.count(d) == 0) {
        NoSuchFile(dir);
    }
    std::vector<std::string> names;
    for (const auto& [path, inode] : live_) {
        if (Parent(path) == d) {
            names.push_back(path.substr(d == "/" ? 1 : (d.empty() ? 0 : d.size() + 1)));
        }
    }
    for (const std::string& sub : dirs_) { // sub-directories are entries too
        if (!sub.empty() && sub != "/" && sub != d && Parent(sub) == d) {
            names.push_back(sub.substr(d == "/" ? 1 : (d.empty() ? 0 : d.size() + 1)));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

void MemoryFileSystem::SyncDirectory(const std::string& dir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string d = Trim(dir);
    if (dirs_.count(d) == 0) {
        NoSuchFile(dir);
    }
    std::vector<DirOp> rest;
    for (DirOp& op : dir_ops_) {
        if (op.dir != d) {
            rest.push_back(std::move(op));
            continue;
        }
        switch (op.kind) {
        case DirOp::Kind::Link:
            durable_[op.a] = op.inode;
            break;
        case DirOp::Kind::Rename:
            durable_.erase(op.a);
            durable_[op.b] = op.inode;
            break;
        case DirOp::Kind::Unlink:
            durable_.erase(op.a);
            break;
        }
    }
    dir_ops_ = std::move(rest);
}

namespace {
class MemoryLock final : public FileLock {
  public:
    template <class State>
    MemoryLock(std::shared_ptr<State> state, std::string path)
        : release_([state = std::move(state), path = std::move(path)] {
              const std::lock_guard<std::mutex> lock(state->mutex);
              state->held.erase(path);
          }) {}
    ~MemoryLock() override { release_(); }

  private:
    std::function<void()> release_;
};
} // namespace

std::unique_ptr<FileLock> MemoryFileSystem::Lock(const std::string& path) {
    {
        const std::lock_guard<std::mutex> lock(locks_->mutex);
        if (!locks_->held.insert(path).second) {
            throw Error(ErrorCode::Io, "the database is in use by another connection or process "
                                       "(could not lock '" +
                                           path + "')");
        }
    }
    return std::make_unique<MemoryLock>(locks_, path);
}

std::shared_ptr<MemoryFileSystem> MemoryFileSystem::Crash(const CrashPolicy& policy) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    Rng rng(policy.seed);
    auto out = std::make_shared<MemoryFileSystem>();
    out->dirs_ = dirs_;

    // 1. which names survive
    std::map<std::string, std::shared_ptr<Inode>> names = durable_;
    size_t keep_ops = 0;
    switch (policy.kind) {
    case CrashPolicy::Kind::DropUnsynced:
        break;
    case CrashPolicy::Kind::KeepAll:
        keep_ops = dir_ops_.size();
        break;
    case CrashPolicy::Kind::Random:
        keep_ops = rng.UpTo(dir_ops_.size());
        break;
    }
    for (size_t i = 0; i < keep_ops; i++) {
        const DirOp& op = dir_ops_[i];
        switch (op.kind) {
        case DirOp::Kind::Link:
            names[op.a] = op.inode;
            break;
        case DirOp::Kind::Rename:
            names.erase(op.a);
            names[op.b] = op.inode;
            break;
        case DirOp::Kind::Unlink:
            names.erase(op.a);
            break;
        }
    }

    // 2. what each surviving file contains
    for (const auto& [path, inode] : names) {
        std::vector<uint8_t> bytes = inode->durable;
        switch (policy.kind) {
        case CrashPolicy::Kind::DropUnsynced:
            break;
        case CrashPolicy::Kind::KeepAll:
            bytes = inode->data;
            break;
        case CrashPolicy::Kind::Random: {
            const auto& ops = inode->pending;
            if (policy.reorder_writes) {
                for (const Inode::Op& op : ops) {
                    if (rng.Coin()) {
                        ApplyOp(bytes, op.truncate, op.at, op.bytes.data(), op.bytes.size());
                    } else if (!op.truncate && rng.UpTo(3) == 0) { // a torn write
                        ApplyOp(bytes, false, op.at, op.bytes.data(), rng.UpTo(op.bytes.size()));
                    }
                }
            } else {
                const size_t kept = rng.UpTo(ops.size());
                for (size_t i = 0; i < kept; i++) {
                    ApplyOp(bytes, ops[i].truncate, ops[i].at, ops[i].bytes.data(),
                            ops[i].bytes.size());
                }
                if (kept < ops.size() && !ops[kept].truncate && rng.Coin()) { // the torn one
                    ApplyOp(bytes, false, ops[kept].at, ops[kept].bytes.data(),
                            rng.UpTo(ops[kept].bytes.size()));
                }
            }
            break;
        }
        }
        auto fresh = std::make_shared<Inode>();
        fresh->data = bytes;
        fresh->durable = std::move(bytes);
        out->live_[path] = fresh;
        out->durable_[path] = fresh;
    }
    return out;
}

std::vector<uint8_t> MemoryFileSystem::Contents(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(path);
    if (it == live_.end()) {
        NoSuchFile(path);
    }
    return it->second->data;
}

void MemoryFileSystem::SetContents(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto inode = std::make_shared<Inode>();
    inode->data = bytes;
    inode->durable = bytes;
    live_[path] = inode;
    durable_[path] = inode;
}

std::vector<std::string> MemoryFileSystem::Paths() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> paths;
    for (const auto& [path, inode] : live_) {
        paths.push_back(path);
    }
    return paths;
}

size_t MemoryFileSystem::UnsyncedOperations() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    size_t n = dir_ops_.size();
    for (const auto& [path, inode] : live_) {
        n += inode->pending.size();
    }
    return n;
}

// ---------------------------------------------------------------------------------- FaultInjector

class FaultInjector::Handle final : public FileHandle {
  public:
    Handle(FaultInjector& owner, std::unique_ptr<FileHandle> inner)
        : owner_(owner), inner_(std::move(inner)) {}
    uint64_t Size() const override {
        owner_.Reading("size");
        return inner_->Size();
    }
    void ReadAt(uint64_t offset, void* dst, size_t n) const override {
        owner_.Reading("read");
        inner_->ReadAt(offset, dst, n);
    }
    void WriteAt(uint64_t offset, const void* src, size_t n) override {
        owner_.Mutating("write");
        inner_->WriteAt(offset, src, n);
    }
    void Truncate(uint64_t size) override {
        owner_.Mutating("truncate");
        inner_->Truncate(size);
    }
    void Sync() override {
        owner_.Mutating("fsync");
        inner_->Sync();
    }

  private:
    FaultInjector& owner_;
    std::unique_ptr<FileHandle> inner_;
};

FaultInjector::FaultInjector(std::shared_ptr<FileSystem> inner) : inner_(std::move(inner)) {}

void FaultInjector::Reset() {
    operations_ = 0;
    crash_at_ = kNever;
    fail_at_ = kNever;
    crashed_ = false;
}

void FaultInjector::Mutating(const char* what) {
    if (crashed_.load()) {
        throw Error(ErrorCode::Io, std::string("simulated crash (the process is gone): ") + what);
    }
    const uint64_t i = operations_.fetch_add(1);
    if (i == crash_at_.load()) {
        crashed_ = true;
        throw Error(ErrorCode::Io,
                    std::string("simulated crash at operation ") + std::to_string(i) + ": " + what);
    }
    if (i == fail_at_.load()) {
        throw Error(ErrorCode::Io, std::string("injected I/O error at operation ") +
                                       std::to_string(i) + ": " + what);
    }
}

void FaultInjector::Reading(const char* what) {
    if (crashed_.load()) {
        throw Error(ErrorCode::Io, std::string("simulated crash (the process is gone): ") + what);
    }
}

std::unique_ptr<FileHandle> FaultInjector::Open(const std::string& path, OpenMode mode) {
    if (mode == OpenMode::Read) {
        Reading("open");
    } else {
        Mutating("open for writing");
    }
    return std::make_unique<Handle>(*this, inner_->Open(path, mode));
}
bool FaultInjector::Exists(const std::string& path) {
    Reading("exists");
    return inner_->Exists(path);
}
bool FaultInjector::Remove(const std::string& path) {
    Mutating("remove");
    return inner_->Remove(path);
}
void FaultInjector::Rename(const std::string& from, const std::string& to) {
    Mutating("rename");
    inner_->Rename(from, to);
}
void FaultInjector::CreateDirectories(const std::string& path) {
    Mutating("create directories");
    inner_->CreateDirectories(path);
}
std::vector<std::string> FaultInjector::List(const std::string& dir) {
    Reading("list");
    return inner_->List(dir);
}
void FaultInjector::SyncDirectory(const std::string& dir) {
    Mutating("directory fsync");
    inner_->SyncDirectory(dir);
}
std::unique_ptr<FileLock> FaultInjector::Lock(const std::string& path) {
    Reading("lock");
    return inner_->Lock(path);
}

} // namespace cdb
