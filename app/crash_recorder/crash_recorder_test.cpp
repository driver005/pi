#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <csignal>
#include <cstdlib>
#include <fstream>

import std;
import pi.base.posix_file_system;
import pi.base.system_clock;
import pi.crash_recorder;

class CrashRecorderTest : public testing::Test {
protected:
    CrashRecorderTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/crash_recorder_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
    }

    nlohmann::json crashes() {
        std::ifstream in(m_dir + "/crashes.json");
        return nlohmann::json::parse(in, nullptr, false);
    }

    std::string m_dir;
    PosixFileSystem m_files;
    SystemClock m_clock;
};

TEST_F(CrashRecorderTest, RecordsACrashWithItsBacktraceAndAnnouncesItOnce) {
    CrashRecorder recorder(m_files, m_clock, m_dir, "/work");
    recorder.setSessionFile("/s/session.jsonl");
    ASSERT_TRUE(recorder.record("fatal_error", "test crash"));
    const auto records = crashes();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0]["kind"], "fatal_error");
    EXPECT_EQ(records[0]["sessionFile"], "/s/session.jsonl");
    EXPECT_EQ(records[0]["cwd"], "/work");
    EXPECT_NE(records[0]["stack"].get<std::string>().find("    at "), std::string::npos);
    const std::string notice = recorder.notice();
    EXPECT_NE(notice.find("pi crashed last time (fatal_error: test crash"), std::string::npos);
    EXPECT_TRUE(recorder.notice().empty());
}

TEST_F(CrashRecorderTest, AFatalSignalIsRecordedBeforeTheProcessDies) {
    EXPECT_EXIT(
        {
            CrashRecorder recorder(m_files, m_clock, m_dir, "/work");
            recorder.install();
            std::raise(SIGABRT);
        },
        testing::KilledBySignal(SIGABRT), "");
    const auto records = crashes();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0]["kind"], "fatal_error");
    EXPECT_NE(records[0]["message"].get<std::string>().find("Abort"), std::string::npos);
}

TEST_F(CrashRecorderTest, TerminateIsRecordedAsAnUncaughtException) {
    EXPECT_EXIT(
        {
            CrashRecorder recorder(m_files, m_clock, m_dir, "/work");
            recorder.install();
            std::terminate();
        },
        testing::KilledBySignal(SIGABRT), "");
    const auto records = crashes();
    ASSERT_GE(records.size(), 1U);
    EXPECT_EQ(records[0]["kind"], "uncaught_exception");
}
