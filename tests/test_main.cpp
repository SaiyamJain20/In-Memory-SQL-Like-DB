// Custom gtest main. Death tests abort() on purpose; on machines whose core_pattern pipes to a
// crash handler (apport / systemd-coredump) every such abort costs ~1 s, and RLIMIT_CORE=0 does
// not stop piped handlers. Marking the process non-dumpable does.
#include <gtest/gtest.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

int main(int argc, char** argv) {
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
