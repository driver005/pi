#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.live_editor;

class LiveEditorTest : public ::testing::Test {
protected:
    LiveEditor m_editor;
};

TEST_F(LiveEditorTest, ToolSlotsAndFinishing) {
    Json live = Json::parse(R"({"tools":[{"callId":"a","status":"pending"},{"callId":"b","taskId":7,"status":"running","output":"x","details":1,"diagnostics":[]}]})");
    EXPECT_TRUE(m_editor.toolSlot(live, 3) == nullptr);
    Json* slot = m_editor.toolSlot(live, 7);
    ASSERT_TRUE(slot != nullptr);
    m_editor.finishSlot(*slot, 42);
    EXPECT_EQ(live.at("tools")[1].dump(), R"({"callId":"b","taskId":7,"status":"done","entry":42})");
    m_editor.finishSlot(*slot, std::nullopt);
    EXPECT_EQ(live.at("tools")[1].at("entry"), 42);
}

TEST_F(LiveEditorTest, CompactionStatusesStayInOrderAndTheListVanishesWhenEmpty) {
    Json live = Json::object();
    m_editor.addCompactionStatus(live, Json::object({{"taskId", 1}}));
    m_editor.addCompactionStatus(live, Json::object({{"taskId", 2}}));
    EXPECT_TRUE(m_editor.compactionStatus(live, 2) != nullptr);
    m_editor.removeCompactionStatus(live, 1);
    EXPECT_EQ(live.at("compactions").size(), 1u);
    m_editor.removeCompactionStatus(live, 2);
    EXPECT_FALSE(live.contains("compactions"));
    m_editor.removeCompactionStatus(live, 9);
}

TEST_F(LiveEditorTest, EndRunSettlesInputsOnlyForTheOwningTask) {
    auto storage = std::make_shared<MemoryStorage>();
    DurableSession session(storage);
    std::int64_t conversationId = 0;
    std::vector<std::int64_t> submissions;
    ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
        if (!created) {
            return std::unexpected(created.error());
        }
        conversationId = created->at("id").get<std::int64_t>();
        for (int i = 0; i < 2; ++i) {
            auto record = tx.createSubmission(Json::object({{"conversationId", conversationId}, {"type", "write"}, {"status", "queued"}}));
            if (!record) {
                return std::unexpected(record.error());
            }
            submissions.push_back(record->at("id").get<std::int64_t>());
        }
        return {};
    }).has_value());
    Json live = Json::object({{"run", Json::object({{"taskId", 5}, {"inputs", Json::array({submissions[0], submissions[1]})}})},
                              {"generation", Json::object({{"attempt", 1}})},
                              {"tools", Json::array()}});
    const Json settlement = Json::object({{"status", "unanswered"}, {"reason", "aborted"}});
    ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
        Json other = live;
        if (auto ended = m_editor.endRun(tx, other, 99, settlement); !ended) {
            return ended;
        }
        // Another task's run stays, but the presentation of generation and tools always goes.
        EXPECT_TRUE(other.contains("run"));
        EXPECT_FALSE(other.contains("generation"));
        return m_editor.endRun(tx, live, 5, settlement);
    }).has_value());
    EXPECT_FALSE(live.contains("run"));
    for (const std::int64_t id : submissions) {
        auto stored = storage->submission(id);
        ASSERT_TRUE(stored.has_value() && stored->has_value());
        EXPECT_EQ((*stored)->at("status"), "unanswered");
    }
}
