#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.inbox_boundary;

class InboxBoundaryTest : public ::testing::Test {
protected:
    InboxBoundaryTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    DocAddressArgs owner() {
        DocAddressArgs args;
        args.owner = m_conversation;
        return args;
    }

    /** Queues an item of `mode` (steer, followUp or write) with its submission; returns the submission id. */
    std::int64_t queue(const std::string& mode, const Json& payload) {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto record = tx.createSubmission(Json::object({{"conversationId", m_conversation},
                                                            {"type", mode == "write" ? "write" : "input"},
                                                            {"status", "queued"}}));
            if (!record) {
                return std::unexpected(record.error());
            }
            id = record->at("id").get<std::int64_t>();
            auto inbox = tx.doc(m_documents.inbox(), owner());
            if (!inbox) {
                return std::unexpected(inbox.error());
            }
            Json item = Json::object({{"id", id}, {"mode", mode}});
            item[mode == "write" ? "entry" : "content"] = payload;
            (**inbox)["items"].push_back(item);
            return {};
        }).has_value());
        return id;
    }

    BoundaryResult place(const std::string& at, const std::string& steering = "one-at-a-time",
                         const std::string& followUp = "one-at-a-time") {
        BoundaryResult result;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto boundary = m_boundary.prepare(tx, m_conversation, steering, followUp);
            if (!boundary) {
                return std::unexpected(boundary.error());
            }
            auto applied = m_boundary.apply(tx, *boundary, at, 100);
            if (!applied) {
                return std::unexpected(applied.error());
            }
            result = *applied;
            return {};
        }).has_value());
        return result;
    }

    std::string status(std::int64_t id) {
        auto stored = m_storage->submission(id);
        return stored && *stored ? (*stored)->at("status").get<std::string>() : "missing";
    }

    size_t inboxSize() {
        auto snapshot = m_session.snapshot(m_documents.inbox(), owner());
        return snapshot && *snapshot ? (*snapshot)->at("items").size() : 0;
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    InboxBoundary m_boundary;
    BuiltinDocuments m_documents;
    std::int64_t m_conversation = 0;
};

TEST_F(InboxBoundaryTest, PostToolsPlacesWritesAndOneSteerButNoFollowUps) {
    const std::int64_t write = queue("write", Json::object({{"kind", "note"}}));
    const std::int64_t steerOne = queue("steer", "first");
    const std::int64_t steerTwo = queue("steer", "second");
    const std::int64_t followUp = queue("followUp", "later");
    BoundaryResult result = place("postTools");
    EXPECT_EQ(result.users, std::vector<std::int64_t>({steerOne}));
    EXPECT_FALSE(result.reset);
    EXPECT_EQ(status(write), "done");
    EXPECT_EQ(status(steerOne), "placed");
    EXPECT_EQ(status(steerTwo), "queued");
    EXPECT_EQ(status(followUp), "queued");
    EXPECT_EQ(inboxSize(), 2u);
}

TEST_F(InboxBoundaryTest, FinalPlacesFollowUpsAndAllModeTakesEveryItem) {
    const std::int64_t steerOne = queue("steer", "a");
    const std::int64_t steerTwo = queue("steer", "b");
    const std::int64_t followOne = queue("followUp", "c");
    const std::int64_t followTwo = queue("followUp", "d");
    BoundaryResult result = place("final", "all", "one-at-a-time");
    EXPECT_EQ(result.users, std::vector<std::int64_t>({steerOne, steerTwo, followOne}));
    EXPECT_EQ(status(followTwo), "queued");
    EXPECT_EQ(inboxSize(), 1u);
}

TEST_F(InboxBoundaryTest, QueuedResetTurnsPostToolsIntoFinalAndPlacesWritesBeforeUsers) {
    const std::int64_t followUp = queue("followUp", "after reset");
    const std::int64_t reset = queue("write", Json::object({{"kind", "pi.reset"}, {"head", "self"}}));
    BoundaryResult result = place("postTools");
    EXPECT_TRUE(result.reset);
    EXPECT_EQ(result.users, std::vector<std::int64_t>({followUp}));
    EXPECT_EQ(status(reset), "done");
    EXPECT_EQ(status(followUp), "placed");
    // The reset entry is the newest head marker, so the user entry placed after it is inside the new context.
    auto marker = m_storage->findLatestHeadMarker(m_conversation, std::nullopt);
    ASSERT_TRUE(marker.has_value() && marker->has_value());
    EXPECT_EQ((*marker)->at("head"), (*marker)->at("id"));
}

TEST_F(InboxBoundaryTest, StaleHeadWritesSettleUnanswered) {
    // An earlier reset moved the active range forward.
    queue("write", Json::object({{"kind", "pi.reset"}, {"head", "self"}}));
    place("final");
    auto marker = m_storage->findLatestHeadMarker(m_conversation, std::nullopt);
    ASSERT_TRUE(marker.has_value() && marker->has_value());
    const std::int64_t resetId = (*marker)->at("id").get<std::int64_t>();
    const std::int64_t stale = queue("write", Json::object({{"kind", "pi.compaction"}, {"head", resetId - 1}}));
    place("final");
    EXPECT_EQ(status(stale), "unanswered");
    auto stored = m_storage->submission(stale);
    EXPECT_EQ((*stored)->at("reason"), "stale");
}

TEST_F(InboxBoundaryTest, WithdrawSettlesInputsAbortedAndKeepsWrites) {
    const std::int64_t write = queue("write", Json::object({{"kind", "note"}}));
    const std::int64_t steer = queue("steer", "x");
    const std::int64_t followUp = queue("followUp", "y");
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_boundary.withdrawQueuedInputs(tx, m_conversation); }).has_value());
    EXPECT_EQ(status(write), "queued");
    EXPECT_EQ(status(steer), "unanswered");
    EXPECT_EQ(status(followUp), "unanswered");
    EXPECT_EQ((*m_storage->submission(steer))->at("reason"), "aborted");
    EXPECT_EQ(inboxSize(), 1u);
}

TEST_F(InboxBoundaryTest, RemoveItemDropsOnlyThatItem) {
    const std::int64_t first = queue("steer", "x");
    queue("steer", "y");
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_boundary.removeItem(tx, m_conversation, first); }).has_value());
    EXPECT_EQ(inboxSize(), 1u);
}
