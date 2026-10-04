#include <gtest/gtest.h>

import std;
import pi.support.task_records;

class TaskRecordsTest : public ::testing::Test {
protected:
    Json record(const std::string& status, const Json& extra = Json::object()) {
        Json task = Json::object({{"id", 5}, {"conversationId", 1}, {"kind", "k"}, {"version", 2}, {"background", false},
                                  {"abortRequested", false}, {"memos", Json::object({{"a", 1}})}});
        task["state"] = Json::object({{"status", status}});
        for (const auto& entry : extra.items()) {
            task[entry.key()] = entry.value();
        }
        return task;
    }

    TaskRecords m_records;
};

TEST_F(TaskRecordsTest, WithStateDropsMemosOnceAnOutcomeIsDecided) {
    Json pending = m_records.withState(record("pending"), Json::object({{"status", "running"}, {"checkpoint", Json::object()}}));
    EXPECT_TRUE(pending.contains("memos"));
    Json done = m_records.withState(pending, Json::object({{"status", "completing"}, {"outcome", Json::object({{"status", "completed"}})}}));
    EXPECT_FALSE(done.contains("memos"));
    EXPECT_EQ(done.at("state").at("status"), "completing");
}

TEST_F(TaskRecordsTest, FailedOutcomeAndCancellationIntent) {
    Json failed = record("completing");
    failed["state"]["outcome"] = Json::object({{"status", "failed"}});
    EXPECT_TRUE(m_records.failedOutcome(failed));
    EXPECT_TRUE(m_records.cancellationIntent(failed));
    Json completed = record("completing");
    completed["state"]["outcome"] = Json::object({{"status", "completed"}});
    EXPECT_FALSE(m_records.failedOutcome(completed));
    EXPECT_FALSE(m_records.cancellationIntent(completed));
    EXPECT_TRUE(m_records.cancellationIntent(record("running", Json::object({{"abortRequested", true}}))));
    Json terminalMarked = record("terminal", Json::object({{"abortRequested", true}}));
    terminalMarked["state"]["outcome"] = Json::object({{"status", "aborted"}});
    EXPECT_FALSE(m_records.cancellationIntent(terminalMarked));
}

TEST_F(TaskRecordsTest, ParentIsOwnerTaskOrConversation) {
    OwnershipRef top = m_records.parentOf(record("pending"));
    EXPECT_FALSE(top.task);
    EXPECT_EQ(top.id, 1);
    OwnershipRef child = m_records.parentOf(record("pending", Json::object({{"owner", 9}})));
    EXPECT_TRUE(child.task);
    EXPECT_EQ(child.id, 9);
}

TEST_F(TaskRecordsTest, MemoAndReservation) {
    EXPECT_EQ(*m_records.memoOf(record("pending"), "a"), 1);
    EXPECT_FALSE(m_records.memoOf(record("pending"), "toString").has_value());
    TaskDefinition same;
    same.version = 2;
    TaskDefinition older;
    older.version = 1;
    TaskDefinition newer;
    newer.version = 3;
    EXPECT_TRUE(m_records.canReserve(same, record("pending")));
    EXPECT_FALSE(m_records.canReserve(older, record("pending")));
    EXPECT_FALSE(m_records.canReserve(newer, record("pending")));
    newer.migrate = [](const Json&, const Json&, std::int64_t) -> Result<Json> { return Json::object(); };
    EXPECT_TRUE(m_records.canReserve(newer, record("pending")));
}
