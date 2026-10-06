// The FileSystem contract, run against the real file system and against MemoryFileSystem, so the
// in-memory model the crash tests rely on behaves like the thing it stands in for.

#include "common/error.h"
#include "io/file_system.h"
#include "io/memory_file_system.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>

#include <unistd.h>

namespace cdb {

namespace {

std::string Read(FileHandle& h, uint64_t offset, size_t n) {
    std::string out(n, '\0');
    h.ReadAt(offset, out.data(), n);
    return out;
}

void Write(FileHandle& h, uint64_t offset, const std::string& s) {
    h.WriteAt(offset, s.data(), s.size());
}

// A directory to work in, and the file system to use. For Posix a fresh temp directory removed at
// the end; for memory a fresh MemoryFileSystem with one directory.
struct Env {
    std::shared_ptr<FileSystem> fs;
    std::string dir;
    std::filesystem::path real;
};

Env MakePosix() {
    Env e;
    e.fs = PosixFileSystem();
    e.real = std::filesystem::temp_directory_path() /
             ("cdb_fs_test_" + std::to_string(::getpid()) + "_" +
              std::to_string(reinterpret_cast<uintptr_t>(&e) & 0xffff));
    std::filesystem::remove_all(e.real);
    e.dir = e.real.string();
    e.fs->CreateDirectories(e.dir);
    return e;
}

Env MakeMemory() {
    Env e;
    e.fs = std::make_shared<MemoryFileSystem>();
    e.dir = "/db";
    e.fs->CreateDirectories(e.dir);
    return e;
}

class FileSystemContract : public ::testing::TestWithParam<bool> {
  protected:
    void SetUp() override { env_ = GetParam() ? MakePosix() : MakeMemory(); }
    void TearDown() override {
        if (!env_.real.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(env_.real, ec);
        }
    }
    FileSystem& fs() { return *env_.fs; }
    std::string P(const std::string& name) const { return JoinPath(env_.dir, name); }
    Env env_;
};

INSTANTIATE_TEST_SUITE_P(Systems, FileSystemContract, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& p) {
                             return p.param ? "Posix" : "Memory";
                         });

} // namespace

TEST(JoinPath, DoesNotDoubleTheSlash) {
    EXPECT_EQ(JoinPath("a", "b"), "a/b");
    EXPECT_EQ(JoinPath("a/", "b"), "a/b");
    EXPECT_EQ(JoinPath("", "b"), "b");
    EXPECT_EQ(JoinPath("/", "b"), "/b");
}

TEST_P(FileSystemContract, WriteReadSizeRoundTrip) {
    auto h = fs().Open(P("a"), OpenMode::Create);
    EXPECT_EQ(h->Size(), 0U);
    Write(*h, 0, "hello");
    Write(*h, 5, " world");
    EXPECT_EQ(h->Size(), 11U);
    EXPECT_EQ(Read(*h, 0, 11), "hello world");
    EXPECT_EQ(Read(*h, 6, 5), "world");
    EXPECT_EQ(Read(*h, 11, 0), "") << "an empty read at the end is fine";
    Write(*h, 2, "LL");
    EXPECT_EQ(Read(*h, 0, 11), "heLLo world");
    h->Sync();
}

TEST_P(FileSystemContract, AReadPastTheEndIsAnIoError) {
    auto h = fs().Open(P("a"), OpenMode::Create);
    Write(*h, 0, "abc");
    char buf[8];
    try {
        h->ReadAt(1, buf, 3); // bytes 1..3, one past the end
        FAIL() << "expected an error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Io);
    }
    EXPECT_THROW(h->ReadAt(100, buf, 1), Error);
}

TEST_P(FileSystemContract, AGapReadsAsZeros) {
    auto h = fs().Open(P("a"), OpenMode::Create);
    Write(*h, 10, "x");
    EXPECT_EQ(h->Size(), 11U);
    EXPECT_EQ(Read(*h, 0, 10), std::string(10, '\0'));
    EXPECT_EQ(Read(*h, 10, 1), "x");
}

TEST_P(FileSystemContract, TruncateShrinksAndGrows) {
    auto h = fs().Open(P("a"), OpenMode::Create);
    Write(*h, 0, "0123456789");
    h->Truncate(4);
    EXPECT_EQ(h->Size(), 4U);
    EXPECT_EQ(Read(*h, 0, 4), "0123");
    h->Truncate(8);
    EXPECT_EQ(h->Size(), 8U);
    EXPECT_EQ(Read(*h, 4, 4), std::string(4, '\0')) << "growing zero-fills";
    h->Truncate(0);
    EXPECT_EQ(h->Size(), 0U);
}

TEST_P(FileSystemContract, OpenModes) {
    EXPECT_THROW(fs().Open(P("missing"), OpenMode::Read), Error);
    EXPECT_THROW(fs().Open(P("missing"), OpenMode::ReadWrite), Error);
    {
        auto h = fs().Open(P("a"), OpenMode::Create);
        Write(*h, 0, "data");
        h->Sync();
    }
    {
        auto h = fs().Open(P("a"), OpenMode::Read);
        EXPECT_EQ(Read(*h, 0, 4), "data");
        EXPECT_THROW(Write(*h, 0, "x"), Error) << "a read-only handle cannot write";
    }
    {
        auto h = fs().Open(P("a"), OpenMode::ReadWrite);
        EXPECT_EQ(h->Size(), 4U) << "ReadWrite keeps the contents";
        Write(*h, 4, "!");
    }
    {
        auto h = fs().Open(P("a"), OpenMode::Create);
        EXPECT_EQ(h->Size(), 0U) << "Create truncates an existing file";
    }
}

TEST_P(FileSystemContract, ExistsRemoveRename) {
    EXPECT_FALSE(fs().Exists(P("a")));
    EXPECT_FALSE(fs().Remove(P("a"))) << "removing a missing file is not an error";
    {
        auto h = fs().Open(P("a"), OpenMode::Create);
        Write(*h, 0, "A");
    }
    {
        auto h = fs().Open(P("b"), OpenMode::Create);
        Write(*h, 0, "B");
    }
    EXPECT_TRUE(fs().Exists(P("a")));
    fs().Rename(P("a"), P("c"));
    EXPECT_FALSE(fs().Exists(P("a")));
    EXPECT_TRUE(fs().Exists(P("c")));
    fs().Rename(P("c"), P("b")); // replaces
    EXPECT_FALSE(fs().Exists(P("c")));
    auto h = fs().Open(P("b"), OpenMode::Read);
    EXPECT_EQ(Read(*h, 0, 1), "A");
    EXPECT_TRUE(fs().Remove(P("b")));
    EXPECT_FALSE(fs().Exists(P("b")));
    EXPECT_THROW(fs().Rename(P("nothing"), P("x")), Error);
}

TEST_P(FileSystemContract, ListIsSortedAndShowsNamesOnly) {
    EXPECT_TRUE(fs().List(env_.dir).empty());
    for (const char* n : {"zeta", "alpha", "mid.tmp", "Beta"}) {
        fs().Open(P(n), OpenMode::Create);
    }
    EXPECT_EQ(fs().List(env_.dir), (std::vector<std::string>{"Beta", "alpha", "mid.tmp", "zeta"}));
    fs().Remove(P("mid.tmp"));
    EXPECT_EQ(fs().List(env_.dir).size(), 3U);
    EXPECT_THROW(fs().List(P("nonexistent_dir")), Error);
}

TEST_P(FileSystemContract, NestedDirectoriesAreCreatedAndSyncable) {
    const std::string nested = P("x/y/z");
    fs().CreateDirectories(nested);
    fs().CreateDirectories(nested); // already there: fine
    fs().Open(JoinPath(nested, "f"), OpenMode::Create);
    EXPECT_EQ(fs().List(nested), std::vector<std::string>{"f"});
    fs().SyncDirectory(nested);
    fs().SyncDirectory(env_.dir);
    EXPECT_THROW(fs().Open(P("no_such_dir/f"), OpenMode::Create), Error);
}

TEST_P(FileSystemContract, ALockIsExclusiveAndReleasedOnDestruction) {
    auto first = fs().Lock(P("LOCK"));
    try {
        fs().Lock(P("LOCK"));
        FAIL() << "a second lock must fail";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Io);
        EXPECT_NE(std::string(e.what()).find("in use"), std::string::npos) << e.what();
    }
    first.reset();
    EXPECT_NO_THROW(fs().Lock(P("LOCK")));
}

TEST_P(FileSystemContract, LargeWritesAndStridedReadsAreExact) {
    std::vector<uint8_t> data(3 * 1024 * 1024 + 17);
    uint32_t x = 12345;
    for (uint8_t& b : data) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    auto h = fs().Open(P("big"), OpenMode::Create);
    h->WriteAt(0, data.data(), data.size());
    h->Sync();
    EXPECT_EQ(h->Size(), data.size());
    std::vector<uint8_t> back(data.size());
    h->ReadAt(0, back.data(), back.size());
    EXPECT_EQ(back, data);
    for (size_t offset : {size_t{1}, size_t{4095}, size_t{1} << 20, data.size() - 5}) {
        uint8_t five[5];
        h->ReadAt(offset, five, 5);
        EXPECT_EQ(std::memcmp(five, data.data() + offset, 5), 0);
    }
}

TEST_P(FileSystemContract, ManyThreadsReadOneHandle) {
    auto h = fs().Open(P("shared"), OpenMode::Create);
    std::string content(100000, 'x');
    for (size_t i = 0; i < content.size(); i++) {
        content[i] = static_cast<char>('a' + i % 26);
    }
    Write(*h, 0, content);
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; t++) {
        threads.emplace_back([&, t] {
            for (size_t i = 0; i < 2000; i++) {
                const size_t off = (i * 37 + static_cast<size_t>(t) * 1009) % (content.size() - 50);
                if (Read(*h, off, 50) != content.substr(off, 50)) {
                    bad++;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(bad.load(), 0);
}

} // namespace cdb
