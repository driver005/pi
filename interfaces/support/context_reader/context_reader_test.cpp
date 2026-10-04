#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.context_reader;

class ContextReaderTest : public ::testing::Test {
protected:
    ContextReaderTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    Json user(const std::string& text) {
        return Json::object({{"role", "user"}, {"content", text}, {"timestamp", 1}});
    }

    Json assistant(const std::string& text, const std::string& stop = "stop", const Json& calls = Json::array()) {
        Json content = Json::array({Json::object({{"type", "text"}, {"text", text}})});
        for (const Json& call : calls) {
            content.push_back(call);
        }
        return Json::object({{"role", "assistant"}, {"content", content}, {"stopReason", stop}, {"timestamp", 2}});
    }

    Json call(const std::string& id) {
        return Json::object({{"type", "toolCall"}, {"id", id}, {"name", "read"}, {"arguments", Json::object()}});
    }

    Json result(const std::string& id) {
        return Json::object({{"role", "toolResult"}, {"toolCallId", id}, {"toolName", "read"},
                             {"content", Json::array()}, {"isError", false}, {"timestamp", 3}});
    }

    std::int64_t append(const Json& draft) {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto entry = tx.appendEntry(m_conversation, draft);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            id = entry->at("id").get<std::int64_t>();
            return {};
        }).has_value());
        return id;
    }

    std::int64_t appendMessage(const Json& message, const Json& extra = Json::object()) {
        Json draft = Json::object({{"kind", "m"}, {"model", Json::array({message})}});
        for (const auto& item : extra.items()) {
            draft[item.key()] = item.value();
        }
        return append(draft);
    }

    Json view(const std::optional<std::int64_t>& at = std::nullopt) {
        auto read = m_reader.read(m_session, m_conversation, at);
        EXPECT_TRUE(read.has_value()) << (read ? "" : read.error().message);
        return read ? *read : Json::object();
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    ContextReader m_reader;
    std::int64_t m_conversation = 0;
};

TEST_F(ContextReaderTest, EmptyConversationHasEmptyContext) {
    Json empty = view();
    EXPECT_TRUE(empty.at("head").is_null());
    EXPECT_TRUE(empty.at("messages").empty());
}

TEST_F(ContextReaderTest, ReadsEntriesOldestFirstAndCanCutOffAtAnEntry) {
    const std::int64_t first = appendMessage(user("one"));
    appendMessage(assistant("two"));
    appendMessage(user("three"));
    EXPECT_EQ(view().at("messages").size(), 3u);
    Json cut = view(first);
    ASSERT_EQ(cut.at("messages").size(), 1u);
    EXPECT_EQ(cut.at("messages")[0].at("content"), "one");
    auto missing = m_reader.read(m_session, m_conversation, 999999);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find("is not visible"), std::string::npos);
}

TEST_F(ContextReaderTest, HeadMarkerStartsTheActiveRange) {
    appendMessage(user("old"));
    const std::int64_t kept = appendMessage(assistant("kept"));
    appendMessage(user("after kept"));
    appendMessage(user("summary"), Json::object({{"head", kept}}));
    appendMessage(user("newest"));
    Json active = view();
    // Head marker, then the non-head entries from its head through the tail.
    std::vector<std::string> texts;
    for (const Json& message : active.at("messages")) {
        const Json& content = message.at("content");
        texts.push_back(content.is_string() ? content.get<std::string>() : content[0].at("text").get<std::string>());
    }
    EXPECT_EQ(texts, std::vector<std::string>({"summary", "kept", "after kept", "newest"}));
    EXPECT_FALSE(active.at("head").is_null());
}

TEST_F(ContextReaderTest, EditsOmitOrReplaceAndTheNewestEditWins) {
    const std::int64_t target = appendMessage(user("original"));
    appendMessage(assistant("answer"));
    append(Json::object({{"kind", "edit"}, {"edits", Json::array({Json::object({{"target", target}, {"action", "omit"}})})}}));
    EXPECT_EQ(view().at("messages").size(), 1u);
    append(Json::object({{"kind", "edit"}, {"edits", Json::array({Json::object({{"target", target}, {"action", "replace"}, {"messages", Json::array({user("replaced")})}})})}}));
    Json replaced = view();
    ASSERT_EQ(replaced.at("messages").size(), 2u);
    EXPECT_EQ(replaced.at("messages")[0].at("content"), "replaced");
}

TEST_F(ContextReaderTest, FailedAssistantMessagesLeaveTheContext) {
    appendMessage(user("q"));
    appendMessage(assistant("partial", "aborted"));
    appendMessage(assistant("err", "error"));
    appendMessage(assistant("deferred", "deferred"));
    appendMessage(assistant("fine", "toolUse"));
    Json active = view();
    EXPECT_EQ(active.at("messages").size(), 2u);
    EXPECT_EQ(active.at("contributions").size(), 5u);
}

TEST_F(ContextReaderTest, ToolResultsFollowTheirAssistantInCallOrderAndMissingOnesAreSynthesized) {
    appendMessage(user("go"));
    appendMessage(assistant("calling", "toolUse", Json::array({call("a"), call("b")})));
    appendMessage(result("b"));
    appendMessage(result("stray"));
    Json messages = view().at("messages");
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[2].at("toolCallId"), "a");
    EXPECT_EQ(messages[2].at("details").at("reason"), "missing_result");
    EXPECT_EQ(messages[2].at("isError"), true);
    EXPECT_EQ(messages[3].at("toolCallId"), "b");
}
