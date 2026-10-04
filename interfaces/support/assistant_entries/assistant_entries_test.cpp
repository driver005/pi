#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.assistant_entries;
import pi.support.durable_session;

class AssistantEntriesTest : public ::testing::Test {
protected:
    AssistantEntriesTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    Json message(const std::string& stop) {
        return Json::object({{"role", "assistant"}, {"content", Json::array()}, {"stopReason", stop}, {"provider", "p"}, {"model", "m"},
                             {"usage", Json::object({{"input", 3}, {"output", 4}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", 7},
                                                     {"cost", Json::object({{"input", 0.0}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", 0.0}})}})},
                             {"timestamp", 1}});
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    AssistantEntries m_entries;
    BuiltinDocuments m_documents;
    std::int64_t m_conversation = 0;
};

TEST_F(AssistantEntriesTest, AppendWritesTheEntryAndTheLedgerInOneCommit) {
    Json entry;
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
        auto appended = m_entries.append(tx, m_conversation, message("stop"));
        if (!appended) {
            return std::unexpected(appended.error());
        }
        entry = *appended;
        return {};
    }).has_value());
    EXPECT_EQ(entry.at("kind"), "pi.assistant");
    EXPECT_EQ(entry.at("model")[0].at("stopReason"), "stop");
    DocAddressArgs args;
    args.owner = m_conversation;
    auto usage = m_session.snapshot(m_documents.usage(), args);
    ASSERT_TRUE(usage.has_value() && usage->has_value());
    EXPECT_EQ((*usage)->at("models").at("p/m").at("totalTokens"), 7);
}

TEST_F(AssistantEntriesTest, ConvertPartialAppendsAnAbortedCopyOnlyWhenThereIsAPartial) {
    Json live = Json::object({{"generation", Json::object({{"attempt", 1}, {"message", message("stop")}})}});
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_entries.convertPartial(tx, live, m_conversation); }).has_value());
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_entries.convertPartial(tx, Json::object({{"generation", Json::object({{"attempt", 2}})}}), m_conversation); }).has_value());
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_entries.convertPartial(tx, Json::object(), m_conversation); }).has_value());
    auto page = m_storage->scanEntries(EntryQuery{m_conversation, std::nullopt, std::nullopt}, 10, std::nullopt);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->items.size(), 1u);
    EXPECT_EQ(page->items[0].at("model")[0].at("stopReason"), "aborted");
}
