#include <gtest/gtest.h>

import std;
import pi.support.crash_log;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;

class CrashLogTest : public testing::Test {
protected:
    FakeFileSystem m_files;
    FixedClock m_clock{1'760'000'000'000};
    CrashLog m_log{m_files, m_clock};
    const std::string m_path = "/agent/crashes.json";
};

TEST_F(CrashLogTest, MissingOrBrokenFilesAreEmptyLogs) {
    EXPECT_TRUE(m_log.read(m_path).empty());
    m_files.createDirectories("/agent");
    m_files.writeFile(m_path, "not json");
    EXPECT_TRUE(m_log.read(m_path).empty());
    m_files.writeFile(m_path, R"([{"timestamp":"2025-01-01T00:00:00.000Z"},{"timestamp":"2025-01-01T00:00:00.000Z","message":"ok"},5])");
    ASSERT_EQ(m_log.read(m_path).size(), 1U);
}

TEST_F(CrashLogTest, RecordsKeepTheNewestFive) {
    for (int i = 0; i < 7; ++i) {
        m_clock.advance(1000);
        ASSERT_TRUE(m_log.record(m_path, "fatal_error", "crash " + std::to_string(i), "stack", "/s.jsonl", "/work").has_value());
    }
    const auto records = m_log.read(m_path);
    ASSERT_EQ(records.size(), 5U);
    EXPECT_EQ(records.front().message, "crash 2");
    EXPECT_EQ(records.back().message, "crash 6");
    EXPECT_EQ(records.back().kind, "fatal_error");
    EXPECT_EQ(records.back().stack, "stack");
    EXPECT_EQ(records.back().sessionFile, "/s.jsonl");
    EXPECT_EQ(records.back().cwd, "/work");
    EXPECT_FALSE(records.back().version.empty());
}

TEST_F(CrashLogTest, TheNewestRecentUnannouncedCrashIsTakenOnce) {
    m_log.record(m_path, "fatal_error", "old", std::nullopt, std::nullopt, "/w");
    m_clock.advance(8LL * 24 * 3600 * 1000);
    m_log.record(m_path, "uncaught_exception", "recent", std::nullopt, std::nullopt, "/w");
    const auto taken = m_log.takeUnnotified(m_path);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->message, "recent");
    EXPECT_FALSE(m_log.takeUnnotified(m_path).has_value());
    for (const CrashRecord& record : m_log.read(m_path)) {
        EXPECT_TRUE(record.notified);
    }
}

TEST_F(CrashLogTest, OldCrashesAreNotAnnouncedAndClearRemovesTheFile) {
    m_log.record(m_path, "fatal_error", "old", std::nullopt, std::nullopt, "/w");
    m_clock.advance(8LL * 24 * 3600 * 1000);
    EXPECT_FALSE(m_log.takeUnnotified(m_path).has_value());
    m_log.clear(m_path);
    EXPECT_FALSE(m_files.exists(m_path));
}
