// The crash model of MemoryFileSystem and the FaultInjector: the foundation every durability test
// stands on, so it is tested on its own first.

#include "common/error.h"
#include "io/memory_file_system.h"

#include <gtest/gtest.h>

#include <set>
#include <thread>

namespace cdb {

namespace {

std::string Str(const std::vector<uint8_t>& b) {
    return {b.begin(), b.end()};
}
void Put(FileHandle& h, uint64_t at, const std::string& s) {
    h.WriteAt(at, s.data(), s.size());
}
std::unique_ptr<MemoryFileSystem> Fresh() {
    auto fs = std::make_unique<MemoryFileSystem>();
    fs->CreateDirectories("/d");
    return fs;
}

// Creates /d/f with `synced` durable (file and name), then returns its handle.
std::unique_ptr<FileHandle> DurableFile(MemoryFileSystem& fs, const std::string& synced) {
    auto h = fs.Open("/d/f", OpenMode::Create);
    Put(*h, 0, synced);
    h->Sync();
    fs.SyncDirectory("/d");
    return h;
}

} // namespace

TEST(MemoryFileSystemCrash, SyncedDataSurvivesAndUnsyncedDataDoesNot) {
    auto fs = Fresh();
    auto h = DurableFile(*fs, "AAAA");
    Put(*h, 4, "BBBB"); // not synced
    const auto dropped = fs->Crash(CrashPolicy::DropUnsynced());
    EXPECT_EQ(Str(dropped->Contents("/d/f")), "AAAA");
    const auto kept = fs->Crash(CrashPolicy::KeepAll());
    EXPECT_EQ(Str(kept->Contents("/d/f")), "AAAABBBB");
    EXPECT_EQ(Str(fs->Contents("/d/f")), "AAAABBBB") << "crashing a copy leaves the original alone";
}

TEST(MemoryFileSystemCrash, ANewFileNeedsADirectorySyncToKeepItsName) {
    auto fs = Fresh();
    auto h = fs->Open("/d/new", OpenMode::Create);
    Put(*h, 0, "data");
    h->Sync(); // the contents are durable, the name is not
    EXPECT_FALSE(fs->Crash(CrashPolicy::DropUnsynced())->Exists("/d/new"));
    EXPECT_TRUE(fs->Crash(CrashPolicy::KeepAll())->Exists("/d/new"));
    fs->SyncDirectory("/d");
    const auto after = fs->Crash(CrashPolicy::DropUnsynced());
    ASSERT_TRUE(after->Exists("/d/new"));
    EXPECT_EQ(Str(after->Contents("/d/new")), "data");
}

TEST(MemoryFileSystemCrash, ANamedFileWhoseDataWasNeverSyncedIsEmptyAfterTheCrash) {
    auto fs = Fresh();
    auto h = fs->Open("/d/new", OpenMode::Create);
    Put(*h, 0, "data");
    fs->SyncDirectory("/d"); // the name is durable, the data is not
    const auto after = fs->Crash(CrashPolicy::DropUnsynced());
    ASSERT_TRUE(after->Exists("/d/new"));
    EXPECT_EQ(after->Contents("/d/new").size(), 0U);
}

TEST(MemoryFileSystemCrash, ARenameIsAtomicAndNeedsADirectorySync) {
    auto fs = Fresh();
    {
        auto old = fs->Open("/d/target", OpenMode::Create);
        Put(*old, 0, "OLD");
        old->Sync();
    }
    auto tmp = fs->Open("/d/target.tmp", OpenMode::Create);
    Put(*tmp, 0, "NEW");
    tmp->Sync();
    fs->SyncDirectory("/d");
    fs->Rename("/d/target.tmp", "/d/target");
    const auto before_sync = fs->Crash(CrashPolicy::DropUnsynced());
    EXPECT_EQ(Str(before_sync->Contents("/d/target")), "OLD")
        << "the old name still points at the old file";
    EXPECT_TRUE(before_sync->Exists("/d/target.tmp"));
    fs->SyncDirectory("/d");
    const auto after_sync = fs->Crash(CrashPolicy::DropUnsynced());
    EXPECT_EQ(Str(after_sync->Contents("/d/target")), "NEW");
    EXPECT_FALSE(after_sync->Exists("/d/target.tmp"));
}

TEST(MemoryFileSystemCrash, ARemoveIsNotDurableUntilTheDirectoryIsSynced) {
    auto fs = Fresh();
    DurableFile(*fs, "keep me");
    fs->Remove("/d/f");
    EXPECT_FALSE(fs->Exists("/d/f"));
    EXPECT_TRUE(fs->Crash(CrashPolicy::DropUnsynced())->Exists("/d/f"));
    fs->SyncDirectory("/d");
    EXPECT_FALSE(fs->Crash(CrashPolicy::DropUnsynced())->Exists("/d/f"));
}

TEST(MemoryFileSystemCrash, RecreatingAFileIsAnUnsyncedTruncation) {
    auto fs = Fresh();
    DurableFile(*fs, "original");
    auto h = fs->Open("/d/f", OpenMode::Create); // truncates
    Put(*h, 0, "new");
    EXPECT_EQ(Str(fs->Crash(CrashPolicy::DropUnsynced())->Contents("/d/f")), "original");
    h->Sync();
    EXPECT_EQ(Str(fs->Crash(CrashPolicy::DropUnsynced())->Contents("/d/f")), "new");
}

TEST(MemoryFileSystemCrash, TruncateIsAnUnsyncedChangeToo) {
    auto fs = Fresh();
    auto h = DurableFile(*fs, "0123456789");
    h->Truncate(3);
    EXPECT_EQ(Str(fs->Crash(CrashPolicy::DropUnsynced())->Contents("/d/f")), "0123456789");
    EXPECT_EQ(Str(fs->Crash(CrashPolicy::KeepAll())->Contents("/d/f")), "012");
    h->Sync();
    EXPECT_EQ(Str(fs->Crash(CrashPolicy::DropUnsynced())->Contents("/d/f")), "012");
}

TEST(MemoryFileSystemCrash, APrefixCrashKeepsASequentialPrefixOfTheLogWithAPossiblyTornTail) {
    auto fs = Fresh();
    auto h = DurableFile(*fs, "AAAA");
    Put(*h, 4, "11111111");
    Put(*h, 12, "22222222");
    Put(*h, 20, "33333333");
    const std::string full =
        "AAAA" + std::string(8, '1') + std::string(8, '2') + std::string(8, '3');
    std::set<std::string> seen;
    for (uint64_t seed = 0; seed < 400; seed++) {
        const std::string got = Str(fs->Crash(CrashPolicy::Random(seed))->Contents("/d/f"));
        EXPECT_EQ(got, full.substr(0, got.size()))
            << "seed " << seed << ": not a prefix of the log";
        EXPECT_GE(got.size(), 4U) << "synced bytes always survive";
        seen.insert(got);
    }
    EXPECT_GE(seen.size(), 8U) << "the seeds should explore many prefixes, including torn ones";
    EXPECT_TRUE(seen.count("AAAA") != 0 && seen.count(full) != 0) << "both extremes are reachable";
    bool torn = false;
    for (const std::string& s : seen) {
        torn = torn || (s.size() > 4 && (s.size() - 4) % 8 != 0);
    }
    EXPECT_TRUE(torn) << "a write torn in the middle should show up";
}

TEST(MemoryFileSystemCrash, AReorderingCrashMayLeaveHolesButNeverInventsBytes) {
    auto fs = Fresh();
    auto h = DurableFile(*fs, "AAAA");
    Put(*h, 4, "11111111");
    Put(*h, 12, "22222222");
    Put(*h, 20, "33333333");
    bool hole = false;
    for (uint64_t seed = 0; seed < 400; seed++) {
        const std::string got = Str(fs->Crash(CrashPolicy::Random(seed, true))->Contents("/d/f"));
        ASSERT_GE(got.size(), 4U);
        EXPECT_EQ(got.substr(0, 4), "AAAA");
        for (size_t i = 4; i < got.size(); i++) {
            const char want = "111111112222222233333333"[i - 4];
            EXPECT_TRUE(got[i] == want || got[i] == '\0') << "byte " << i << " seed " << seed;
        }
        // a later write present while an earlier one is missing
        if (got.size() > 12 && got[4] == '\0') {
            hole = true;
        }
    }
    EXPECT_TRUE(hole) << "reordering should sometimes persist a later write without an earlier one";
}

TEST(MemoryFileSystemCrash, ARandomCrashKeepsAPrefixOfTheDirectoryOperations) {
    // create a, create b, create c (all unsynced): the survivors are a prefix of that order
    std::set<std::string> outcomes;
    for (uint64_t seed = 0; seed < 200; seed++) {
        auto fs = Fresh();
        for (const char* n : {"/d/a", "/d/b", "/d/c"}) {
            auto h = fs->Open(n, OpenMode::Create);
            Put(*h, 0, "x");
            h->Sync();
        }
        const auto crashed = fs->Crash(CrashPolicy::Random(seed));
        std::string present;
        for (const char* n : {"/d/a", "/d/b", "/d/c"}) {
            present += crashed->Exists(n) ? '1' : '0';
        }
        outcomes.insert(present);
    }
    EXPECT_EQ(outcomes, (std::set<std::string>{"000", "100", "110", "111"}));
}

TEST(MemoryFileSystemCrash, TheSameSeedGivesTheSameSurvivors) {
    auto fs = Fresh();
    auto h = DurableFile(*fs, "AAAA");
    for (int i = 0; i < 10; i++) {
        Put(*h, 4 + static_cast<uint64_t>(i) * 5, std::string(5, static_cast<char>('a' + i)));
    }
    for (const bool reorder : {false, true}) {
        for (uint64_t seed = 0; seed < 20; seed++) {
            EXPECT_EQ(fs->Crash(CrashPolicy::Random(seed, reorder))->Contents("/d/f"),
                      fs->Crash(CrashPolicy::Random(seed, reorder))->Contents("/d/f"));
        }
    }
}

TEST(MemoryFileSystemCrash, ACrashedFileSystemIsFreshlyUsable) {
    auto fs = Fresh();
    auto lock = fs->Lock("/d/LOCK");
    auto h = DurableFile(*fs, "data");
    const auto crashed = fs->Crash(CrashPolicy::KeepAll());
    EXPECT_NO_THROW(crashed->Lock("/d/LOCK")) << "locks do not survive a crash";
    auto again = crashed->Open("/d/f", OpenMode::ReadWrite);
    Put(*again, 4, "+more");
    EXPECT_EQ(Str(crashed->Contents("/d/f")), "data+more");
    EXPECT_EQ(Str(fs->Contents("/d/f")), "data") << "and the two file systems are independent";
    EXPECT_EQ(crashed->UnsyncedOperations(), 1U);
}

TEST(MemoryFileSystemCrash, UnsyncedOperationsCountsWhatACrashCouldLose) {
    auto fs = Fresh();
    EXPECT_EQ(fs->UnsyncedOperations(), 0U);
    auto h = fs->Open("/d/f", OpenMode::Create); // a directory operation
    EXPECT_EQ(fs->UnsyncedOperations(), 1U);
    Put(*h, 0, "x");
    Put(*h, 1, "y");
    EXPECT_EQ(fs->UnsyncedOperations(), 3U);
    h->Sync();
    EXPECT_EQ(fs->UnsyncedOperations(), 1U);
    fs->SyncDirectory("/d");
    EXPECT_EQ(fs->UnsyncedOperations(), 0U);
}

TEST(MemoryFileSystem, SetContentsAndPathsSupportPlantingFiles) {
    auto fs = Fresh();
    fs->SetContents("/d/planted", {1, 2, 3});
    EXPECT_EQ(fs->Contents("/d/planted"), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(fs->Paths(), std::vector<std::string>{"/d/planted"});
    EXPECT_EQ(fs->Crash(CrashPolicy::DropUnsynced())->Contents("/d/planted").size(), 3U)
        << "planted files are durable";
    EXPECT_THROW(fs->Contents("/d/missing"), Error);
}

TEST(MemoryFileSystem, ConcurrentWritersAndReadersOnDifferentFiles) {
    auto fs = Fresh();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&fs, t] {
            auto h = fs->Open("/d/f" + std::to_string(t), OpenMode::Create);
            for (int i = 0; i < 500; i++) {
                Put(*h, static_cast<uint64_t>(i) * 4, "abcd");
                if (i % 50 == 0) {
                    h->Sync();
                }
            }
            std::string back(2000, '\0');
            h->ReadAt(0, back.data(), back.size());
        });
    }
    std::thread crasher([&fs] {
        for (int i = 0; i < 50; i++) {
            fs->Crash(CrashPolicy::Random(static_cast<uint64_t>(i)));
        }
    });
    for (auto& th : threads) {
        th.join();
    }
    crasher.join();
    EXPECT_EQ(fs->List("/d").size(), 4U);
}

// ---------------------------------------------------------------------------------- FaultInjector

TEST(FaultInjector, CountsMutatingOperationsOnly) {
    auto mem = std::make_shared<MemoryFileSystem>();
    FaultInjector fi(mem);
    fi.CreateDirectories("/d");                 // 0
    auto h = fi.Open("/d/f", OpenMode::Create); // 1
    h->WriteAt(0, "abc", 3);                    // 2
    h->Sync();                                  // 3
    fi.SyncDirectory("/d");                     // 4
    char buf[3];
    h->ReadAt(0, buf, 3);
    h->Size();
    fi.Exists("/d/f");
    fi.List("/d");
    fi.Open("/d/f", OpenMode::Read);
    fi.Lock("/d/LOCK");
    EXPECT_EQ(fi.operations(), 5U) << "reads, listings, locks and read-only opens are not counted";
    h->Truncate(1);            // 5
    fi.Rename("/d/f", "/d/g"); // 6
    fi.Remove("/d/g");         // 7
    EXPECT_EQ(fi.operations(), 8U);
    EXPECT_FALSE(fi.crashed());
}

TEST(FaultInjector, ACrashStopsTheOperationAndEverythingAfterIt) {
    auto mem = std::make_shared<MemoryFileSystem>();
    mem->CreateDirectories("/d");
    FaultInjector fi(mem);
    fi.CrashAtOperation(2);
    auto h = fi.Open("/d/f", OpenMode::Create); // 0
    h->WriteAt(0, "abc", 3);                    // 1
    try {
        h->Sync(); // 2: crash
        FAIL() << "the crash point must throw";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Io);
        EXPECT_NE(std::string(e.what()).find("simulated crash"), std::string::npos);
    }
    EXPECT_TRUE(fi.crashed());
    char buf[3];
    EXPECT_THROW(h->ReadAt(0, buf, 3), Error) << "a dead process reads nothing either";
    EXPECT_THROW(h->WriteAt(0, "x", 1), Error);
    EXPECT_THROW(fi.Exists("/d/f"), Error);
    EXPECT_THROW(fi.List("/d"), Error);
    EXPECT_THROW(fi.Rename("/d/f", "/d/g"), Error);
    // the operation that crashed did not happen: the data was written but never synced
    EXPECT_FALSE(mem->Crash(CrashPolicy::DropUnsynced())->Exists("/d/f")) << "name never synced";
}

TEST(FaultInjector, AnInjectedFailureAffectsOnlyThatOperation) {
    auto mem = std::make_shared<MemoryFileSystem>();
    mem->CreateDirectories("/d");
    FaultInjector fi(mem);
    fi.FailOperation(1);
    auto h = fi.Open("/d/f", OpenMode::Create); // 0
    try {
        h->WriteAt(0, "abc", 3); // 1: fails
        FAIL() << "expected an injected error";
    } catch (const Error& e) {
        EXPECT_NE(std::string(e.what()).find("injected"), std::string::npos);
    }
    EXPECT_FALSE(fi.crashed());
    EXPECT_EQ(h->Size(), 0U) << "the failed write did not happen";
    h->WriteAt(0, "abc", 3); // 2: works
    h->Sync();
    EXPECT_EQ(Str(mem->Contents("/d/f")), "abc");
}

TEST(FaultInjector, ResetStartsANewMachine) {
    auto mem = std::make_shared<MemoryFileSystem>();
    mem->CreateDirectories("/d");
    FaultInjector fi(mem);
    fi.CrashAtOperation(0);
    EXPECT_THROW(fi.Open("/d/f", OpenMode::Create), Error);
    EXPECT_TRUE(fi.crashed());
    fi.Reset();
    EXPECT_FALSE(fi.crashed());
    EXPECT_EQ(fi.operations(), 0U);
    EXPECT_NO_THROW(fi.Open("/d/f", OpenMode::Create));
}

TEST(FaultInjector, CountsAreExactUnderConcurrency) {
    auto mem = std::make_shared<MemoryFileSystem>();
    mem->CreateDirectories("/d");
    FaultInjector fi(mem);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&fi, t] {
            auto h = fi.Open("/d/f" + std::to_string(t), OpenMode::Create);
            for (int i = 0; i < 100; i++) {
                h->WriteAt(static_cast<uint64_t>(i), "x", 1);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(fi.operations(), 4U * 101U);
}

} // namespace cdb
