#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.outcome_settlement;

class OutcomeSettlementTest : public ::testing::Test {
protected:
    OutcomeSettlementTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            auto record = tx.createSubmission(Json::object({{"conversationId", m_conversation}, {"type", "input"}, {"status", "queued"}}));
            if (!record) {
                return std::unexpected(record.error());
            }
            m_submission = record->at("id").get<std::int64_t>();
            return {};
        }).has_value());
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) { return tx.placeSubmission(m_submission, 1); }).has_value());
    }

    Json record(const std::string& kind, std::int64_t id) {
        return Json::object({{"id", id}, {"conversationId", m_conversation}, {"kind", kind}});
    }

    void seedLive(const Json& live) {
        ASSERT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            DocAddressArgs args;
            args.owner = m_conversation;
            auto doc = tx.doc(m_documents.live(), args);
            if (!doc) {
                return std::unexpected(doc.error());
            }
            **doc = live;
            return {};
        }).has_value());
    }

    Json live() {
        DocAddressArgs args;
        args.owner = m_conversation;
        auto snapshot = m_session.snapshot(m_documents.live(), args);
        return snapshot && *snapshot ? **snapshot : Json(nullptr);
    }

    Result<std::int64_t> settle(const Json& taskRecord, const Json& outcome) {
        return m_session.commit([&](Transaction& tx) { return m_settlement.settle(tx, taskRecord, outcome); });
    }

    Json runLive(std::int64_t taskId) {
        return Json::object({{"run", Json::object({{"taskId", taskId}, {"inputs", Json::array({m_submission})}})},
                             {"generation", Json::object({{"attempt", 1}, {"message", Json::object({{"role", "assistant"}, {"content", Json::array()}, {"stopReason", "stop"}, {"timestamp", 1}})}})}});
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    OutcomeSettlement m_settlement;
    BuiltinDocuments m_documents;
    std::int64_t m_conversation = 0;
    std::int64_t m_submission = 0;
};

TEST_F(OutcomeSettlementTest, FaultedGenerationEndsTheRunAndKeepsThePartialAsAnAbortedEntry) {
    seedLive(runLive(40));
    ASSERT_TRUE(settle(record("pi.generation", 40), Json::object({{"status", "faulted"}, {"error", Json::object({{"message", "boom"}})}})).has_value());
    auto submission = m_storage->submission(m_submission);
    EXPECT_EQ((*submission)->at("status"), "unanswered");
    EXPECT_EQ((*submission)->at("reason"), "faulted");
    EXPECT_EQ((*submission)->at("detail"), "boom");
    EXPECT_FALSE(live().contains("run"));
    EXPECT_FALSE(live().contains("generation"));
    auto page = m_storage->scanEntries(EntryQuery{m_conversation, std::nullopt, std::nullopt}, 10, std::nullopt);
    ASSERT_EQ(page->items.size(), 1u);
    EXPECT_EQ(page->items[0].at("model")[0].at("stopReason"), "aborted");
}

TEST_F(OutcomeSettlementTest, OrphanedGenerationSettlesWithItsReason) {
    seedLive(runLive(41));
    ASSERT_TRUE(settle(record("pi.generation", 41), Json::object({{"status", "orphaned"}, {"reason", "missing_task"}})).has_value());
    EXPECT_EQ((*m_storage->submission(m_submission))->at("reason"), "missing_task");
}

TEST_F(OutcomeSettlementTest, AGenerationThatDoesNotOwnTheRunChangesNothing) {
    seedLive(runLive(42));
    ASSERT_TRUE(settle(record("pi.generation", 99), Json::object({{"status", "faulted"}, {"error", Json::object({{"message", "x"}})}})).has_value());
    EXPECT_TRUE(live().contains("run"));
    EXPECT_EQ((*m_storage->submission(m_submission))->at("status"), "placed");
}

TEST_F(OutcomeSettlementTest, ToolTasksMarkTheirSlotDoneWithoutAnEntry) {
    seedLive(Json::object({{"tools", Json::array({Json::object({{"callId", "a"}, {"taskId", 7}, {"status", "running"}, {"output", "partial"}})})}}));
    ASSERT_TRUE(settle(record("pi.tool", 7), Json::object({{"status", "faulted"}, {"error", Json::object({{"message", "x"}})}})).has_value());
    EXPECT_EQ(live().at("tools")[0].dump(), R"({"callId":"a","taskId":7,"status":"done"})");
}

TEST_F(OutcomeSettlementTest, CompactionTasksLoseTheirStatus) {
    seedLive(Json::object({{"compactions", Json::array({Json::object({{"taskId", 8}})})}}));
    ASSERT_TRUE(settle(record("pi.compaction", 8), Json::object({{"status", "orphaned"}, {"reason", "task_too_old"}})).has_value());
    EXPECT_FALSE(live().contains("compactions"));
}

TEST_F(OutcomeSettlementTest, OtherKindsNeverCreateTheLiveDocument) {
    auto seq = settle(record("custom", 9), Json::object({{"status", "orphaned"}, {"reason", "missing_task"}}));
    ASSERT_TRUE(seq.has_value());
    EXPECT_EQ(*seq, 0);
    DocAddressArgs args;
    args.owner = m_conversation;
    auto snapshot = m_session.snapshot(m_documents.live(), args);
    EXPECT_FALSE(snapshot->has_value());
}
