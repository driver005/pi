#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.submission_admission;

class SubmissionAdmissionTest : public ::testing::Test {
protected:
    SubmissionAdmissionTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    SubmissionDraft input(const std::string& text, const std::optional<std::string>& whenBusy = std::nullopt,
                          const std::optional<std::string>& requestId = std::nullopt) {
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = text;
        draft.whenBusy = whenBusy;
        draft.requestId = requestId;
        return draft;
    }

    SubmissionDraft write(const Json& entry) {
        SubmissionDraft draft;
        draft.type = "write";
        draft.entry = entry;
        return draft;
    }

    Result<std::int64_t> admit(const SubmissionDraft& draft) {
        std::int64_t id = 0;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto admitted = m_admission.admit(tx, m_conversation, draft, 123, "one-at-a-time", "one-at-a-time");
            if (!admitted) {
                return std::unexpected(admitted.error());
            }
            id = *admitted;
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return id;
    }

    Json submission(std::int64_t id) {
        auto stored = m_storage->submission(id);
        return stored && *stored ? **stored : Json(nullptr);
    }

    Json live() {
        DocAddressArgs args;
        args.owner = m_conversation;
        auto snapshot = m_session.snapshot(m_documents.live(), args);
        return snapshot && *snapshot ? **snapshot : Json::object();
    }

    Json inboxItems() {
        DocAddressArgs args;
        args.owner = m_conversation;
        auto snapshot = m_session.snapshot(m_documents.inbox(), args);
        return snapshot && *snapshot ? (*snapshot)->at("items") : Json::array();
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    SubmissionAdmission m_admission;
    BuiltinDocuments m_documents;
    std::int64_t m_conversation = 0;
};

TEST_F(SubmissionAdmissionTest, IdleInputIsPlacedAndStartsARun) {
    auto id = admit(input("hello"));
    ASSERT_TRUE(id.has_value()) << id.error().message;
    Json record = submission(*id);
    EXPECT_EQ(record.at("status"), "placed");
    EXPECT_EQ(record.at("type"), "input");
    Json run = live().at("run");
    EXPECT_EQ(run.at("inputs").dump(), "[" + std::to_string(*id) + "]");
    auto entry = m_storage->entry(record.at("entry").get<std::int64_t>());
    ASSERT_TRUE(entry.has_value() && entry->has_value());
    EXPECT_EQ((*entry)->entry.at("model")[0].at("content"), "hello");
    EXPECT_EQ((*entry)->entry.at("model")[0].at("timestamp"), 123);
    auto task = m_storage->task(run.at("taskId").get<std::int64_t>());
    ASSERT_TRUE(task.has_value() && task->has_value());
    EXPECT_EQ((*task)->at("kind"), "pi.generation");
}

TEST_F(SubmissionAdmissionTest, BusyConversationQueuesBySteerOrFollowUpOrRejects) {
    ASSERT_TRUE(admit(input("first")).has_value());
    auto follow = admit(input("later"));
    auto steer = admit(input("now", "steer"));
    ASSERT_TRUE(follow.has_value() && steer.has_value());
    EXPECT_EQ(submission(*follow).at("status"), "queued");
    Json items = inboxItems();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].at("mode"), "followUp");
    EXPECT_EQ(items[1].at("mode"), "steer");
    EXPECT_EQ(items[1].at("content"), "now");
    auto rejected = admit(input("no", "reject"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, "conversation_busy");
    EXPECT_EQ(inboxItems().size(), 2u);
}

TEST_F(SubmissionAdmissionTest, BusyWriteIsQueuedWithItsEntry) {
    ASSERT_TRUE(admit(input("first")).has_value());
    auto queued = admit(write(Json::object({{"kind", "note"}})));
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(submission(*queued).at("type"), "write");
    EXPECT_EQ(submission(*queued).at("status"), "queued");
    EXPECT_EQ(inboxItems()[0].at("mode"), "write");
}

TEST_F(SubmissionAdmissionTest, RequestIdsDeduplicateWithinAConversation) {
    auto first = admit(input("a", std::nullopt, "req-1"));
    auto again = admit(input("b", std::nullopt, "req-1"));
    ASSERT_TRUE(first.has_value() && again.has_value());
    EXPECT_EQ(*first, *again);
    auto mismatch = admit(write(Json::object({{"kind", "note"}})));
    ASSERT_TRUE(mismatch.has_value());
    SubmissionDraft wrongType = write(Json::object({{"kind", "note"}}));
    wrongType.requestId = "req-1";
    auto failed = admit(wrongType);
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().message.find("already identifies a submission of type input"), std::string::npos);
}

TEST_F(SubmissionAdmissionTest, IdleWriteAppendsAndSettlesDoneOrStale) {
    auto done = admit(write(Json::object({{"kind", "note"}})));
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(submission(*done).at("status"), "done");
    EXPECT_TRUE(live().empty());
    EXPECT_TRUE(m_storage->entry(submission(*done).at("entry").get<std::int64_t>())->has_value());
    // A reset moves the active range forward; a later head write that reaches before it is stale.
    auto reset = admit(write(Json::object({{"kind", "pi.reset"}, {"head", "self"}})));
    ASSERT_TRUE(reset.has_value());
    const std::int64_t resetEntry = submission(*reset).at("entry").get<std::int64_t>();
    auto stale = admit(write(Json::object({{"kind", "pi.compaction"}, {"head", resetEntry - 1}})));
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(submission(*stale).at("status"), "unanswered");
    EXPECT_EQ(submission(*stale).at("reason"), "stale");
}
