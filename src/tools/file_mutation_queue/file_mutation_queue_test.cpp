#include <gtest/gtest.h>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.tools.file_mutation_queue;

class FileMutationQueueTest : public testing::Test {
protected:
    PosixFileSystem m_fs;
    FileMutationQueue m_queue{m_fs};
    std::string m_dir = std::string(std::getenv("TEST_TMPDIR"));
};

TEST_F(FileMutationQueueTest, SameFileIsSerialized) {
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&] {
            m_queue.run(m_dir + "/same", [&] {
                const int now = ++inside;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                --inside;
            });
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(peak.load(), 1);
    EXPECT_EQ(m_queue.trackedFiles(), 0U);
}

TEST_F(FileMutationQueueTest, DifferentFilesRunInParallel) {
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&, i] {
            m_queue.run(m_dir + "/file" + std::to_string(i), [&] {
                const int now = ++inside;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                --inside;
            });
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_GE(peak.load(), 2);
}

TEST_F(FileMutationQueueTest, SymlinkAndTargetShareOneLock) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/target", "x").has_value());
    std::filesystem::remove(m_dir + "/alias");
    std::filesystem::create_symlink(m_dir + "/target", m_dir + "/alias");
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    auto work = [&](const std::string& path) {
        m_queue.run(path, [&] {
            const int now = ++inside;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            --inside;
        });
    };
    std::thread first(work, m_dir + "/target");
    std::thread second(work, m_dir + "/alias");
    first.join();
    second.join();
    EXPECT_EQ(peak.load(), 1);
}
