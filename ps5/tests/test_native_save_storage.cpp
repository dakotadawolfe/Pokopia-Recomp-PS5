// SPDX-License-Identifier: GPL-3.0-or-later
// Runs in the actual app before game startup; payload libc differs from native.
#include "test_framework.h"
#include "nxrt/kernel.h"
#include "../../runtime/src/services/fs_volume.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

NXTEST_GROUP(native_save_storage)
NXTEST(native_title_storage_commits_recovers_and_rolls_back) {
    namespace fs = nxrt::services::fs;
    std::string directory = "/download0/pokopia-save-check-XXXXXX";
    REQUIRE(mkdtemp(directory.data()) != nullptr);
    struct Cleanup {
        std::string directory;
        ~Cleanup() {
            fs::Volume::set_crash_point_for_tests(0);
            const auto root = directory + "/save";
            const auto work = root + ".work." + std::to_string(getpid());
            for (const auto& path : {root + "/progress", work + "/progress", root + ".commit",
                 root + ".journal." + std::to_string(getpid()), root + ".lock"}) unlink(path.c_str());
            rmdir(work.c_str()); rmdir(root.c_str());
            CHECK_EQ(rmdir(directory.c_str()), 0);
        }
    } cleanup{directory};
    const auto space = fs::available_storage(directory);
    std::printf("SAVE_STORAGE_CHECK available=%llu measured=%d errno=%d\n", static_cast<unsigned long long>(space.bytes),
        space.status == fs::SpaceStatus::Measured, space.error);
    REQUIRE(space.status != fs::SpaceStatus::Error);
    if (space.status == fs::SpaceStatus::Measured) REQUIRE(space.bytes >= (128ull << 20));
    const auto root = directory + "/save";
    const auto put = [](fs::Volume& v, const char* text) {
        std::shared_ptr<fs::VFile> f; if (v.open_file("/progress", 7, &f)) return false;
        return !f->write(0, text, std::strlen(text)) && !f->set_size(std::strlen(text));
    };
    const auto read = [](fs::Volume& v) {
        std::shared_ptr<fs::VFile> f; if (v.open_file("/progress", 1, &f)) return std::string("open failed");
        char bytes[64]{}; uint64_t count = 0;
        if (f->read(0, bytes, sizeof bytes, &count)) return std::string("read failed");
        return std::string(bytes, count);
    };
    {
        std::printf("SAVE_STORAGE_CHECK stage=commit\n");
        fs::Volume v(root, true, false, 4096); REQUIRE(v.ok());
        REQUIRE(v.create_file("/progress", 0) == 0); REQUIRE(put(v, "committed")); REQUIRE(v.commit() == 0);
        REQUIRE(put(v, "discarded"));
    }
    {
        std::printf("SAVE_STORAGE_CHECK stage=rollback-and-interruption\n");
        fs::Volume v(root, true, false, 4096); REQUIRE(v.ok()); REQUIRE(read(v) == "committed");
        REQUIRE(put(v, "recovered")); fs::Volume::set_crash_point_for_tests(1);
        REQUIRE(v.commit() != 0); fs::Volume::set_crash_point_for_tests(0);
    }
    {
        std::printf("SAVE_STORAGE_CHECK stage=recovery\n");
        fs::Volume v(root, true, false, 4096); REQUIRE(v.ok()); REQUIRE(read(v) == "recovered");
        fs::FileTimes times; REQUIRE(v.file_times("/progress", &times) == 0); REQUIRE(times.valid);
    }
    std::printf("SAVE_STORAGE_CHECK committed=true recovered=true rollback=true\n");
}
NXTEST(native_positive_thread_sleep_really_waits) {
    const auto start = std::chrono::steady_clock::now();
    nxrt::kernel::sleep_ns(20000000);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("SLEEP_STORAGE_CHECK requested_us=20000 elapsed_us=%lld\n", static_cast<long long>(us));
    REQUIRE(us >= 19000);
}
