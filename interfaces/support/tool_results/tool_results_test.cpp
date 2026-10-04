#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.tool_results;

class ToolResultsTest : public ::testing::Test {
protected:
    ToolResultsTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    Json append(const ToolExecutionResult& result) {
        Json entry;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto appended = m_results.append(tx, m_conversation, Json::object({{"id", "call-1"}, {"name", "read"}}), result, 77);
            if (!appended) {
                return std::unexpected(appended.error());
            }
            entry = *appended;
            return {};
        }).has_value());
        return entry;
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    ToolResults m_results;
    std::int64_t m_conversation = 0;
};

TEST_F(ToolResultsTest, ResultWithoutDiagnosticsStoresContentAndDefaults) {
    ToolExecutionResult result;
    result.content = Json::parse(R"([{"type":"text","text":"ok"}])");
    result.details = Json::object({{"k", 1}});
    Json entry = append(result);
    EXPECT_EQ(entry.at("kind"), "pi.tool-result");
    const Json& message = entry.at("model")[0];
    EXPECT_EQ(message.at("role"), "toolResult");
    EXPECT_EQ(message.at("toolCallId"), "call-1");
    EXPECT_EQ(message.at("toolName"), "read");
    EXPECT_EQ(message.at("content").size(), 1u);
    EXPECT_EQ(message.at("isError"), false);
    EXPECT_EQ(message.at("timestamp"), 77);
    EXPECT_EQ(message.at("details").at("k"), 1);
    EXPECT_TRUE(entry.at("data").at("diagnostics").empty());
}

TEST_F(ToolResultsTest, DiagnosticsAreRenderedLastInTheContentAndKeptStructured) {
    ToolExecutionResult result = m_results.harnessError("blocked", "Tool call blocked: no");
    result.diagnostics.push_back(m_results.diagnostic("warn", "truncated", "Output truncated"));
    Json entry = append(result);
    const Json& message = entry.at("model")[0];
    EXPECT_EQ(message.at("isError"), true);
    ASSERT_EQ(message.at("content").size(), 1u);
    EXPECT_EQ(message.at("content")[0].at("text"),
              "<harness>\n[error] Tool call blocked: no\n[warn] Output truncated\n</harness>");
    ASSERT_EQ(entry.at("data").at("diagnostics").size(), 2u);
    EXPECT_EQ(entry.at("data").at("diagnostics")[0].at("code"), "blocked");
    ToolDiagnostic roundTrip = m_results.diagnosticFromJson(entry.at("data").at("diagnostics")[1]);
    EXPECT_EQ(roundTrip.severity, "warn");
    EXPECT_EQ(*roundTrip.code, "truncated");
}

TEST_F(ToolResultsTest, UsageGoesIntoTheToolBucketOfTheLedger) {
    ToolExecutionResult result;
    result.content = Json::array();
    result.usage = Json::object({{"input", 5}, {"output", 1}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", 6},
                                 {"cost", Json::object({{"input", 0.5}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", 0.5}})}});
    append(result);
    append(result);
    BuiltinDocuments documents;
    DocAddressArgs args;
    args.owner = m_conversation;
    auto snapshot = m_session.snapshot(documents.usage(), args);
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("tools").at("read").at("input"), 10);
}

TEST_F(ToolResultsTest, JsonRoundTripKeepsEveryField) {
    ToolExecutionResult result = m_results.harnessError("c", "m");
    result.details = Json::object({{"a", 1}});
    result.usage = Json::object({{"input", 1}});
    result.control = Json::object({{"terminate", true}});
    result.content = Json::array();
    ToolExecutionResult back = m_results.fromJson(m_results.toJson(result));
    EXPECT_EQ(back.isError, std::optional<bool>(true));
    EXPECT_EQ(back.details, result.details);
    EXPECT_EQ(back.usage, result.usage);
    EXPECT_EQ(back.control, result.control);
    ASSERT_EQ(back.diagnostics.size(), 1u);
    EXPECT_EQ(*back.diagnostics[0].code, "c");
    ToolExecutionResult bare = m_results.fromJson(Json::object());
    EXPECT_FALSE(bare.content.has_value());
    EXPECT_FALSE(bare.isError.has_value());
    EXPECT_TRUE(bare.diagnostics.empty());
}
