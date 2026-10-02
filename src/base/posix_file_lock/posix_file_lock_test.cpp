#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <cstdlib>

import std;
import pi.base.posix_file_lock;

class PosixFileLockTest : public testing::Test {
protected:
    std::string m_path = std::string(std::getenv("TEST_TMPDIR")) + "/file_lock_target";
    PosixFileLock m_lock;
};

TEST_F(PosixFileLockTest, RunsActionAndPropagatesResult) {
    int calls = 0;
    const auto ok = m_lock.withLock(m_path, [&]() -> Result<void> {
        ++calls;
        return {};
    });
    EXPECT_TRUE(ok.has_value());
    const auto failed = m_lock.withLock(
        m_path, []() -> Result<void> { return std::unexpected(Error{"x", "boom"}); });
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "boom");
    EXPECT_EQ(calls, 1);
}

TEST_F(PosixFileLockTest, SerializesConcurrentActionsOnSamePath) {
    std::atomic<int> inside{0};
    std::atomic<int> maxInside{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&] {
            PosixFileLock lock;
            lock.withLock(m_path, [&]() -> Result<void> {
                const int now = ++inside;
                int seen = maxInside.load();
                while (now > seen && !maxInside.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                --inside;
                return {};
            });
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(maxInside.load(), 1);
}

TEST_F(PosixFileLockTest, ReportsUnopenableLockFile) {
    const auto result = m_lock.withLock("/nonexistent-dir/file", []() -> Result<void> { return {}; });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "lock_open");
}

TEST_F(PosixFileLockTest, LockIsADirectoryWhileHeldAndRemovedAfter) {
    const std::string lockPath = m_path + ".lock";
    struct stat info {};
    bool wasDirectory = false;
    m_lock.withLock(m_path, [&]() -> Result<void> {
        wasDirectory = stat(lockPath.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        return {};
    });
    EXPECT_TRUE(wasDirectory);
    EXPECT_NE(stat(lockPath.c_str(), &info), 0);
}

TEST_F(PosixFileLockTest, StaleLockDirectoryIsBroken) {
    const std::string lockPath = m_path + ".lock";
    ASSERT_EQ(mkdir(lockPath.c_str(), 0700), 0);
    // Pretend another process died 60s ago while holding the lock.
    struct timeval old[2];
    gettimeofday(&old[0], nullptr);
    old[0].tv_sec -= 60;
    old[1] = old[0];
    ASSERT_EQ(utimes(lockPath.c_str(), old), 0);
    bool ran = false;
    const auto result = m_lock.withLock(m_path, [&]() -> Result<void> {
        ran = true;
        return {};
    });
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(ran);
}
