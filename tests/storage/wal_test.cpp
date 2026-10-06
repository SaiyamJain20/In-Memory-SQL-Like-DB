// The write-ahead log file: framing, torn tails, damage, and what a crash can leave behind. The
// property that matters: whatever happened to the bytes, the transactions recovered are a prefix of
// the transactions that were committed, intact - never a changed one, never one that was not
// committed, and (after a power cut) never fewer than the ones that were fsynced.

#include "storage/wal.h"

#include "io/memory_file_system.h"

#include <gtest/gtest.h>

#include <random>

namespace cdb {

namespace {

constexpr const char* kPath = "/db/wal-0000000000000000.log";

std::shared_ptr<MemoryFileSystem> NewFs() {
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->CreateDirectories("/db");
    return fs;
}

using Payload = std::vector<uint8_t>;
// A transaction: its frames' payloads, the last of which carries the commit flag.
using Txn = std::vector<Payload>;

Payload Bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

void Append(WalWriter& wal, const Txn& txn) {
    for (size_t i = 0; i < txn.size(); i++) {
        wal.AppendFrame(txn[i], i + 1 == txn.size());
    }
}

// Reads what a scan says is committed, grouped back into transactions by... the log does not mark
// transaction boundaries in the payloads, so return the flat list of frame payloads.
std::vector<Payload> Replay(FileSystem& fs, const WalScan& scan, uint64_t epoch = 0) {
    (void)epoch;
    std::vector<Payload> out;
    ReplayWal(fs, kPath, scan, [&](const uint8_t* p, size_t n) { out.emplace_back(p, p + n); });
    return out;
}

std::vector<Payload> Flatten(const std::vector<Txn>& txns, size_t count) {
    std::vector<Payload> out;
    for (size_t i = 0; i < count; i++) {
        for (const Payload& p : txns[i]) {
            out.push_back(p);
        }
    }
    return out;
}

// Cumulative file offsets after each committed transaction.
std::vector<uint64_t> Boundaries(const std::vector<Txn>& txns) {
    std::vector<uint64_t> ends;
    uint64_t pos = kWalHeaderSize;
    for (const Txn& t : txns) {
        for (const Payload& p : t) {
            pos += kWalFrameHeaderSize + p.size();
        }
        ends.push_back(pos);
    }
    return ends;
}

std::vector<Txn> SampleTxns() {
    return {
        {Bytes("first")},
        {Bytes(""), Bytes("second, in two frames")},
        {Bytes(std::string(300, 'x'))},
        {Bytes("a"), Bytes("b"), Bytes("c")},
        {Bytes("")},
        {Bytes("last one")},
    };
}

std::shared_ptr<MemoryFileSystem> WriteLog(const std::vector<Txn>& txns, bool sync = true) {
    auto fs = NewFs();
    auto wal = WalWriter::Create(*fs, kPath, 0);
    for (const Txn& t : txns) {
        Append(*wal, t);
    }
    if (sync) {
        wal->Sync();
    }
    return fs;
}

void Plant(MemoryFileSystem& fs, const std::vector<uint8_t>& bytes) {
    fs.SetContents(kPath, bytes);
}

} // namespace

TEST(Wal, AFreshLogHoldsOnlyItsHeader) {
    auto fs = NewFs();
    auto wal = WalWriter::Create(*fs, kPath, 7);
    EXPECT_EQ(wal->size(), kWalHeaderSize);
    EXPECT_EQ(wal->next_sequence(), 0U);
    const WalScan scan = ScanWal(*fs, kPath, 7);
    EXPECT_TRUE(scan.header_valid);
    EXPECT_EQ(scan.valid_size, kWalHeaderSize);
    EXPECT_EQ(scan.committed_frames, 0U);
    EXPECT_EQ(scan.discarded_bytes(), 0U);
    EXPECT_TRUE(Replay(*fs, scan).empty());
}

TEST(Wal, CommittedTransactionsComeBackInOrderAndIntact) {
    const std::vector<Txn> txns = SampleTxns();
    auto fs = WriteLog(txns);
    const WalScan scan = ScanWal(*fs, kPath, 0);
    EXPECT_EQ(scan.valid_size, fs->Contents(kPath).size());
    EXPECT_EQ(scan.committed_transactions, txns.size());
    EXPECT_EQ(scan.committed_frames, 1U + 2 + 1 + 3 + 1 + 1);
    EXPECT_EQ(scan.next_sequence, scan.committed_frames);
    EXPECT_EQ(scan.discarded_frames, 0U);
    EXPECT_EQ(Replay(*fs, scan), Flatten(txns, txns.size()));
    EXPECT_EQ(Boundaries(txns).back(), scan.valid_size)
        << "the format is exactly what the docs say";
}

TEST(Wal, FramesOfATransactionThatNeverCommittedAreDiscarded) {
    auto fs = NewFs();
    auto wal = WalWriter::Create(*fs, kPath, 0);
    wal->AppendFrame(Bytes("committed"), true);
    wal->AppendFrame(Bytes("part one of an unfinished transaction"), false);
    wal->AppendFrame(Bytes("part two"), false);
    wal->Sync();
    const WalScan scan = ScanWal(*fs, kPath, 0);
    EXPECT_EQ(scan.committed_transactions, 1U);
    EXPECT_EQ(scan.committed_frames, 1U);
    EXPECT_EQ(scan.discarded_frames, 2U);
    EXPECT_GT(scan.discarded_bytes(), 0U);
    EXPECT_EQ(Replay(*fs, scan), std::vector<Payload>{Bytes("committed")});
}

TEST(Wal, EveryTruncationRecoversExactlyTheTransactionsThatFitCompletely) {
    const std::vector<Txn> txns = SampleTxns();
    auto fs = WriteLog(txns);
    const std::vector<uint8_t> good = fs->Contents(kPath);
    const std::vector<uint64_t> ends = Boundaries(txns);
    ASSERT_EQ(ends.back(), good.size());
    for (size_t cut = 0; cut <= good.size(); cut++) {
        Plant(*fs, std::vector<uint8_t>(good.begin(), good.begin() + static_cast<long>(cut)));
        const WalScan scan = ScanWal(*fs, kPath, 0);
        size_t complete = 0;
        uint64_t boundary = kWalHeaderSize;
        while (complete < ends.size() && ends[complete] <= cut) {
            boundary = ends[complete++];
        }
        if (cut < kWalHeaderSize) {
            EXPECT_FALSE(scan.header_valid) << cut;
            EXPECT_TRUE(Replay(*fs, scan).empty());
            continue;
        }
        ASSERT_TRUE(scan.header_valid) << cut;
        EXPECT_EQ(scan.valid_size, boundary) << "cut at " << cut;
        EXPECT_EQ(scan.committed_transactions, complete) << "cut at " << cut;
        EXPECT_EQ(scan.discarded_bytes(), cut - boundary);
        EXPECT_EQ(Replay(*fs, scan), Flatten(txns, complete)) << "cut at " << cut;
    }
}

TEST(Wal, EverySingleBitFlipYieldsAnIntactPrefixOrAClearError) {
    const std::vector<Txn> txns = {{Bytes("alpha")},
                                   {Bytes("beta"), Bytes("gamma")},
                                   {Bytes("")},
                                   {Bytes(std::string(40, 'z'))}};
    auto fs = WriteLog(txns);
    const std::vector<uint8_t> good = fs->Contents(kPath);
    for (size_t byte = 0; byte < good.size(); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            std::vector<uint8_t> damaged = good;
            damaged[byte] ^= static_cast<uint8_t>(1 << bit);
            Plant(*fs, damaged);
            try {
                const WalScan scan = ScanWal(*fs, kPath, 0);
                ASSERT_TRUE(scan.header_valid);
                // whatever was recovered is exactly the first k transactions
                const std::vector<Payload> got = Replay(*fs, scan);
                bool is_prefix = false;
                for (size_t k = 0; k <= txns.size(); k++) {
                    is_prefix = is_prefix || got == Flatten(txns, k);
                }
                ASSERT_TRUE(is_prefix) << "byte " << byte << " bit " << bit
                                       << " produced something that was never committed";
                ASSERT_GE(byte, kWalHeaderSize) << "a flipped header byte must not go unnoticed";
                ASSERT_LT(scan.committed_transactions, txns.size())
                    << "damage inside a frame cannot leave every transaction intact";
            } catch (const Error& e) {
                ASSERT_EQ(e.code(), ErrorCode::Corruption) << e.what();
                ASSERT_LT(byte, kWalHeaderSize)
                    << "only the header may be a hard error: " << e.what();
            }
        }
    }
}

TEST(Wal, AHeaderFromAnotherEpochOrAnotherFileIsCorruption) {
    auto fs = WriteLog({{Bytes("x")}});
    EXPECT_THROW(ScanWal(*fs, kPath, 1), Error) << "wal-0's header says epoch 0";
    try {
        ScanWal(*fs, kPath, 1);
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption);
    }
    Plant(*fs, std::vector<uint8_t>(100, 'q'));
    try {
        ScanWal(*fs, kPath, 0);
        FAIL() << "garbage of real size is not a log";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption);
    }
}

TEST(Wal, ATornOrEmptyHeaderMeansALogThatWasBeingCreated) {
    auto fs = NewFs();
    for (const size_t size : {size_t{0}, size_t{5}, size_t{15}, size_t{16}}) {
        Plant(*fs, std::vector<uint8_t>(size, 0));
        const WalScan scan = ScanWal(*fs, kPath, 0);
        EXPECT_FALSE(scan.header_valid) << size;
        EXPECT_EQ(scan.valid_size, 0U);
        EXPECT_TRUE(Replay(*fs, scan).empty());
    }
}

TEST(Wal, AStaleFrameFromAnEarlierLifeOfTheFileIsNotReplayed) {
    // frames whose sequence numbers do not continue the log (left over after a file was reused)
    auto fs = NewFs();
    auto first = WalWriter::Continue(*fs, kPath, 0, 0, 0); // creates the header
    first->AppendFrame(Bytes("real"), true);
    first->Sync();
    const uint64_t end_of_real = first->size();
    first->AppendFrame(Bytes("sequence 1"), true);
    first->Sync();
    std::vector<uint8_t> bytes = fs->Contents(kPath);
    // splice: header + "real" + a frame carrying sequence 5 (written by a writer that skipped
    // ahead)
    auto other = NewFs();
    auto skipped = WalWriter::Create(*other, kPath, 0);
    skipped->AppendFrame(Bytes("real"), true);
    auto resumed = WalWriter::Continue(*other, kPath, 0, skipped->size(), 5);
    resumed->AppendFrame(Bytes("sequence 5, not 1"), true);
    resumed->Sync();
    const WalScan scan = ScanWal(*other, kPath, 0);
    EXPECT_EQ(scan.committed_transactions, 1U)
        << "the frame with the wrong sequence number stops the scan";
    EXPECT_EQ(scan.valid_size, end_of_real);
    EXPECT_EQ(scan.next_sequence, 1U);
}

TEST(Wal, ContinuingAfterATornTailCutsItOffAndAppendsCleanly) {
    const std::vector<Txn> txns = SampleTxns();
    auto fs = WriteLog(txns);
    std::vector<uint8_t> bytes = fs->Contents(kPath);
    // a torn frame at the end: half of one more transaction
    {
        auto scratch = NewFs();
        auto w = WalWriter::Create(*scratch, kPath, 0);
        for (const Txn& t : txns) {
            Append(*w, t);
        }
        w->AppendFrame(Bytes("this one will be torn"), true);
        w->Sync();
        bytes = scratch->Contents(kPath);
        bytes.resize(bytes.size() - 7);
    }
    Plant(*fs, bytes);
    const WalScan scan = ScanWal(*fs, kPath, 0);
    ASSERT_EQ(scan.committed_transactions, txns.size());
    ASSERT_GT(scan.discarded_bytes(), 0U);
    auto wal = WalWriter::Continue(*fs, kPath, 0, scan.valid_size, scan.next_sequence);
    EXPECT_EQ(fs->Contents(kPath).size(), scan.valid_size) << "the torn tail is gone";
    // the cut is durable: a crash right now still shows the cut file
    EXPECT_EQ(fs->Crash(CrashPolicy::DropUnsynced())->Contents(kPath).size(), scan.valid_size);
    wal->AppendFrame(Bytes("after recovery"), true);
    wal->Sync();
    const WalScan after = ScanWal(*fs, kPath, 0);
    EXPECT_EQ(after.committed_transactions, txns.size() + 1);
    std::vector<Payload> want = Flatten(txns, txns.size());
    want.push_back(Bytes("after recovery"));
    EXPECT_EQ(Replay(*fs, after), want);
}

TEST(Wal, ContinuingAFileWithoutAHeaderStartsItAfresh) {
    auto fs = NewFs();
    Plant(*fs, std::vector<uint8_t>(9, 0xAB)); // a header cut short
    const WalScan scan = ScanWal(*fs, kPath, 3);
    ASSERT_FALSE(scan.header_valid);
    auto wal = WalWriter::Continue(*fs, kPath, 3, scan.valid_size, scan.next_sequence);
    EXPECT_EQ(wal->size(), kWalHeaderSize);
    wal->AppendFrame(Bytes("new"), true);
    wal->Sync();
    const WalScan after = ScanWal(*fs, kPath, 3);
    EXPECT_TRUE(after.header_valid);
    EXPECT_EQ(after.committed_transactions, 1U);
}

TEST(Wal, AFailedWriteOrSyncPoisonsTheWriter) {
    for (const bool fail_write : {true, false}) {
        auto mem = NewFs();
        FaultInjector fi(mem);
        auto wal = WalWriter::Create(fi, kPath, 0); // operations 0 (open), 1 (write), 2 (sync)
        wal->AppendFrame(Bytes("fine"), true);      // 3
        const uint64_t ops = fi.operations();
        fi.FailOperation(fail_write ? ops : ops + 1);
        const uint64_t size_before = wal->size();
        if (fail_write) {
            EXPECT_THROW(wal->AppendFrame(Bytes("fails"), true), Error);
            EXPECT_EQ(wal->size(), size_before) << "a failed append does not advance the log";
        } else {
            wal->AppendFrame(Bytes("written"), true);
            EXPECT_THROW(wal->Sync(), Error);
        }
        EXPECT_TRUE(wal->poisoned());
        for (int i = 0; i < 3; i++) {
            try {
                wal->AppendFrame(Bytes("more"), true);
                FAIL() << "a poisoned log accepts nothing";
            } catch (const Error& e) {
                EXPECT_NE(std::string(e.what()).find("reopened"), std::string::npos) << e.what();
            }
        }
        EXPECT_THROW(wal->Sync(), Error);
    }
}

TEST(Wal, LargeAndEmptyPayloadsRoundTrip) {
    std::mt19937_64 rng(1);
    Payload big(3 * 1024 * 1024 + 5);
    for (uint8_t& b : big) {
        b = static_cast<uint8_t>(rng());
    }
    const std::vector<Txn> txns = {{Payload{}}, {big}, {Payload{}, Payload{}}, {Bytes("end")}};
    auto fs = WriteLog(txns);
    const WalScan scan = ScanWal(*fs, kPath, 0);
    EXPECT_EQ(scan.committed_transactions, 4U);
    EXPECT_EQ(Replay(*fs, scan), Flatten(txns, 4));
}

TEST(Wal, AfterAPowerCutTheRecoveredTransactionsAreAPrefixContainingEverythingFsynced) {
    // Commit transactions one at a time, fsyncing after some of them. Under every crash policy
    // the log that comes back holds at least the fsynced transactions and nothing that was not
    // written, in order and intact.
    std::mt19937_64 rng(2);
    for (uint64_t seed = 0; seed < 300; seed++) {
        auto fs = NewFs();
        fs->SyncDirectory("/db");
        auto wal = WalWriter::Create(*fs, kPath, 0);
        fs->SyncDirectory("/db"); // the name of the log is durable from here on
        std::vector<Txn> txns;
        size_t synced = 0;
        const size_t n = 1 + rng() % 12;
        for (size_t i = 0; i < n; i++) {
            Txn t;
            for (size_t f = 0, frames = 1 + rng() % 3; f < frames; f++) {
                t.push_back(Bytes(std::string(rng() % 200, static_cast<char>('a' + i))));
            }
            Append(*wal, t);
            txns.push_back(t);
            if (rng() % 2 == 0) {
                wal->Sync();
                synced = txns.size();
            }
        }
        for (const CrashPolicy& policy :
             {CrashPolicy::DropUnsynced(), CrashPolicy::KeepAll(), CrashPolicy::Random(seed, false),
              CrashPolicy::Random(seed, true)}) {
            const auto after = fs->Crash(policy);
            const WalScan scan = ScanWal(*after, kPath, 0);
            const std::vector<Payload> got = Replay(*after, scan);
            size_t matched = 0;
            for (size_t k = 0; k <= txns.size(); k++) {
                if (got == Flatten(txns, k)) {
                    matched = k + 1; // k + 1: found
                    EXPECT_GE(k, synced) << "seed " << seed << ": an fsynced transaction was lost";
                    break;
                }
            }
            ASSERT_NE(matched, 0U) << "seed " << seed << ": recovered a log that is not a prefix";
        }
    }
}

} // namespace cdb
