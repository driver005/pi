#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;

class FailingStorage : public MemoryStorage {
public:
    Result<std::int64_t> commit(const std::vector<Json>& writes) override {
        if (m_failure) {
            return std::unexpected(Error{*m_failure, "forced"});
        }
        return MemoryStorage::commit(writes);
    }

    void failWith(const std::string& code) {
        m_failure = code;
    }

    void heal() {
        m_failure.reset();
    }

private:
    std::optional<std::string> m_failure;
};

class DurableSessionTest : public ::testing::Test {
protected:
    DocDefinition counter() {
        DocDefinition def;
        def.kind = "counter";
        def.version = 1;
        def.scope = "conversation";
        def.history = "latest";
        def.fork = "current";
        def.initial = [](const Json&) { return Json::object({{"count", 0}}); };
        return def;
    }

    DocDefinition rewindable() {
        DocDefinition def = counter();
        def.kind = "log";
        def.history = "rewindable";
        def.fork = "asOf";
        return def;
    }

    DocAddressArgs on(std::int64_t owner) {
        DocAddressArgs args;
        args.owner = owner;
        return args;
    }

    std::int64_t newConversation(DurableSession& session) {
        std::int64_t id = 0;
        auto committed = session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        });
        EXPECT_TRUE(committed.has_value());
        return id;
    }

    Result<std::int64_t> increment(DurableSession& session, std::int64_t conversationId, const DocDefinition& def) {
        return session.commit([&](Transaction& tx) -> Result<void> {
            auto draft = tx.doc(def, on(conversationId));
            if (!draft) {
                return std::unexpected(draft.error());
            }
            (**draft)["count"] = (**draft).at("count").get<std::int64_t>() + 1;
            return {};
        });
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
};

TEST_F(DurableSessionTest, CommitSnapshotAndPublication) {
    DurableSession session(m_storage);
    std::vector<Json> publications;
    ASSERT_TRUE(session.subscribeCommits([&](const Json& publication) { publications.push_back(publication); }).has_value());
    const std::int64_t conversationId = newConversation(session);
    ASSERT_TRUE(increment(session, conversationId, counter()).has_value());
    ASSERT_TRUE(increment(session, conversationId, counter()).has_value());

    auto snapshot = session.snapshot(counter(), on(conversationId));
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("count"), 2);
    ASSERT_EQ(publications.size(), 3u);
    EXPECT_EQ(publications[0].at("changes")[0].at("type"), "conversation");
    EXPECT_EQ(publications[2].at("changes")[0].at("type"), "document");
    EXPECT_EQ(publications[2].at("changes")[0].at("ops").size(), 1u);
    EXPECT_LT(publications[1].at("seq").get<std::int64_t>(), publications[2].at("seq").get<std::int64_t>());
}

TEST_F(DurableSessionTest, NoWritesPublishesNothing) {
    DurableSession session(m_storage);
    int count = 0;
    ASSERT_TRUE(session.subscribeCommits([&](const Json&) { ++count; }).has_value());
    auto committed = session.commit([](Transaction&) -> Result<void> { return {}; });
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(*committed, 0);
    EXPECT_EQ(count, 0);
}

TEST_F(DurableSessionTest, FailedCallbackWritesNothing) {
    DurableSession session(m_storage);
    auto committed = session.commit([](Transaction& tx) -> Result<void> {
        auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
        EXPECT_TRUE(created.has_value());
        return std::unexpected(Error{"boom", "callback failed"});
    });
    ASSERT_FALSE(committed.has_value());
    EXPECT_EQ(committed.error().code, "boom");
    auto page = m_storage->scanConversations(ConversationQuery{}, 10, std::nullopt);
    ASSERT_TRUE(page.has_value());
    EXPECT_TRUE(page->items.empty());
}

TEST_F(DurableSessionTest, SnapshotAsOfReadsHistoricalValue) {
    DurableSession session(m_storage);
    const std::int64_t conversationId = newConversation(session);
    std::vector<std::int64_t> entries;
    for (int step = 0; step < 3; ++step) {
        ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
            auto draft = tx.doc(rewindable(), on(conversationId));
            if (!draft) {
                return std::unexpected(draft.error());
            }
            (**draft)["count"] = step + 1;
            auto entry = tx.appendEntry(conversationId, Json::object({{"kind", "message"}}));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            entries.push_back(entry->at("id").get<std::int64_t>());
            return {};
        }).has_value());
    }
    auto early = session.snapshotAsOf(rewindable(), on(conversationId), entries[0]);
    ASSERT_TRUE(early.has_value() && early->has_value());
    EXPECT_EQ((*early)->at("count"), 1);
    auto latest = session.snapshotAsOf(rewindable(), on(conversationId), entries[2]);
    ASSERT_TRUE(latest.has_value() && latest->has_value());
    EXPECT_EQ((*latest)->at("count"), 3);
    EXPECT_FALSE(session.snapshotAsOf(rewindable(), on(conversationId), 99999).has_value());
}

TEST_F(DurableSessionTest, ColdSessionReloadsDocumentsFromStorage) {
    std::int64_t conversationId = 0;
    {
        DurableSession first(m_storage);
        conversationId = newConversation(first);
        ASSERT_TRUE(increment(first, conversationId, counter()).has_value());
        ASSERT_TRUE(increment(first, conversationId, counter()).has_value());
    }
    DurableSession second(m_storage);
    auto snapshot = second.snapshot(counter(), on(conversationId));
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("count"), 2);
    second.unloadDocuments();
    ASSERT_TRUE(increment(second, conversationId, counter()).has_value());
    auto after = second.snapshot(counter(), on(conversationId));
    ASSERT_TRUE(after.has_value() && after->has_value());
    EXPECT_EQ((*after)->at("count"), 3);
}

TEST_F(DurableSessionTest, ConcurrentCommitsAreSerialized) {
    DurableSession session(m_storage);
    const std::int64_t conversationId = newConversation(session);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                EXPECT_TRUE(increment(session, conversationId, counter()).has_value());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    auto snapshot = session.snapshot(counter(), on(conversationId));
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("count"), 200);
}

TEST_F(DurableSessionTest, ListenerMayCommitAndOrderIsPreserved) {
    DurableSession session(m_storage);
    const std::int64_t conversationId = newConversation(session);
    std::vector<std::int64_t> seen;
    bool reacted = false;
    ASSERT_TRUE(session.subscribeCommits([&](const Json& publication) {
        seen.push_back(publication.at("seq").get<std::int64_t>());
        if (!reacted) {
            reacted = true;
            EXPECT_TRUE(session.commit([&](Transaction& tx) { return tx.appendEntry(conversationId, Json::object({{"kind", "reaction"}})).transform([](Json) {}); }).has_value());
        }
    }).has_value());
    ASSERT_TRUE(increment(session, conversationId, counter()).has_value());
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_LT(seen[0], seen[1]);
}

TEST_F(DurableSessionTest, ConversationCreatedHookStagesWritesInTheSameCommit) {
    DocDefinition notes = counter();
    notes.kind = "notes";
    DurableSession session(m_storage, [&](Transaction& tx, const Json& record) -> Result<void> {
        auto draft = tx.doc(notes, on(record.at("id").get<std::int64_t>()));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        (**draft)["count"] = 42;
        return {};
    });
    const std::int64_t conversationId = newConversation(session);
    auto snapshot = session.snapshot(notes, on(conversationId));
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("count"), 42);
}

TEST_F(DurableSessionTest, StorageRejectionKeepsTheSessionUsableOtherFailuresPoisonIt) {
    auto failing = std::make_shared<FailingStorage>();
    DurableSession session(failing);
    const std::int64_t conversationId = newConversation(session);
    failing->failWith("storage_rejected");
    EXPECT_FALSE(increment(session, conversationId, counter()).has_value());
    failing->heal();
    EXPECT_TRUE(increment(session, conversationId, counter()).has_value());
    failing->failWith("storage_error");
    EXPECT_FALSE(increment(session, conversationId, counter()).has_value());
    failing->heal();
    auto poisoned = increment(session, conversationId, counter());
    ASSERT_FALSE(poisoned.has_value());
    EXPECT_EQ(poisoned.error().code, "session_poisoned");
}

TEST_F(DurableSessionTest, CloseSealsAdmissionAndRunsHooksOnce) {
    int hooks = 0;
    int notified = 0;
    DurableSession session(m_storage, nullptr, [&] { ++hooks; });
    ASSERT_TRUE(session.subscribeClose([&] { ++notified; }).has_value());
    ASSERT_TRUE(session.close().has_value());
    ASSERT_TRUE(session.close().has_value());
    EXPECT_EQ(hooks, 1);
    EXPECT_EQ(notified, 1);
    auto after = session.commit([](Transaction&) -> Result<void> { return {}; });
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, "session_closed");
    EXPECT_FALSE(m_storage->mintId().has_value());
}

TEST_F(DurableSessionTest, LineListenersRunOnTheLineBeforeCommitReturnsAndInOrder) {
    DurableSession session(m_storage);
    std::vector<std::int64_t> online;
    std::vector<std::int64_t> later;
    auto handle = session.subscribeCommitsOnLine([&](const Json& publication) { online.push_back(publication.at("seq").get<std::int64_t>()); });
    ASSERT_TRUE(handle.has_value());
    ASSERT_TRUE(session.subscribeCommits([&](const Json& publication) { later.push_back(publication.at("seq").get<std::int64_t>()); }).has_value());
    const std::int64_t conversationId = newConversation(session);
    ASSERT_TRUE(increment(session, conversationId, counter()).has_value());
    // Line listeners saw both commits by the time commit returned.
    EXPECT_EQ(online.size(), 2u);
    EXPECT_EQ(later, online);
    session.unsubscribeCommitsOnLine(*handle);
    ASSERT_TRUE(increment(session, conversationId, counter()).has_value());
    EXPECT_EQ(online.size(), 2u);
    EXPECT_EQ(later.size(), 3u);
}
