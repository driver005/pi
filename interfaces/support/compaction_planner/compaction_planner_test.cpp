#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.compaction_planner;
import pi.support.durable_session;

class CompactionPlannerTest : public ::testing::Test {
protected:
    Json user(const std::string& text) {
        return Json::object({{"role", "user"}, {"content", text}, {"timestamp", 1}});
    }

    Json assistant(const std::string& text, const Json& calls = Json::array(), std::int64_t total = 0) {
        Json content = Json::array({Json::object({{"type", "text"}, {"text", text}})});
        for (const Json& call : calls) {
            content.push_back(call);
        }
        Json usage = Json::object({{"input", total}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", total},
                                   {"cost", Json::object({{"input", 0.0}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", 0.0}})}});
        return Json::object({{"role", "assistant"}, {"content", content}, {"stopReason", "stop"}, {"usage", usage}, {"timestamp", 2}});
    }

    Json call(const std::string& id) {
        return Json::object({{"type", "toolCall"}, {"id", id}, {"name", "read"}, {"arguments", Json::object({{"path", "a.txt"}})}});
    }

    Json result(const std::string& id, const std::string& text) {
        return Json::object({{"role", "toolResult"}, {"toolCallId", id}, {"toolName", "read"},
                             {"content", Json::array({Json::object({{"type", "text"}, {"text", text}})})}, {"isError", false}, {"timestamp", 3}});
    }

    /** A view whose entry i contributes contributions[i] (each a list of messages). */
    Json view(const std::vector<std::vector<Json>>& contributions, const Json& head = Json(nullptr)) {
        Json entries = Json::array();
        Json contributionList = Json::array();
        Json messages = Json::array();
        std::int64_t id = 10;
        for (const auto& contribution : contributions) {
            entries.push_back(Json::object({{"id", id++}, {"kind", "m"}}));
            Json list = Json::array();
            for (const Json& message : contribution) {
                list.push_back(message);
                messages.push_back(message);
            }
            contributionList.push_back(list);
        }
        return Json::object({{"head", head}, {"entries", entries}, {"contributions", contributionList}, {"messages", messages}});
    }

    std::string text(std::size_t chars) {
        return std::string(chars, 'x');
    }

    CompactionPlanner m_planner;
};

TEST_F(CompactionPlannerTest, CutsAtTheFirstCandidateAtOrAfterTheKeepBoundary) {
    Json v = view({{user(text(40))}, {assistant(text(40))}, {user(text(40))}, {assistant(text(40))}});
    // Walking back: 10 tokens (< 15), then 20 tokens at index 2, a user entry.
    EXPECT_EQ(m_planner.selectCut(v, 15), std::optional<std::size_t>(2));
}

TEST_F(CompactionPlannerTest, NothingToCompactWhenTheKeptTailReachesTheStartOrFitsEntirely) {
    EXPECT_FALSE(m_planner.selectCut(view({{user(text(40))}}), 5).has_value());
    EXPECT_FALSE(m_planner.selectCut(view({{user(text(40))}, {assistant(text(40))}}), 1000).has_value());
    EXPECT_FALSE(m_planner.selectCut(view({}), 1).has_value());
}

TEST_F(CompactionPlannerTest, NeverCutsAtAToolResultOrBetweenACallAndItsResult) {
    // [assistant(call a)] [user interjection] [tool result a] [assistant]
    Json v = view({{user(text(40))}, {assistant(text(4), Json::array({call("a")}))}, {user(text(40))}, {result("a", text(40))}, {assistant(text(40))}});
    // The interjection at index 2 is not a candidate (the result of the preceding call follows it); the next is index 4.
    EXPECT_EQ(m_planner.selectCut(v, 15), std::optional<std::size_t>(4));
}

TEST_F(CompactionPlannerTest, TheHeadMarkerIsNotACutCandidateAndEmptyPrefixesDoNotCompact) {
    Json head = Json::object({{"id", 9}, {"kind", "pi.compaction"}, {"head", 10}});
    // Entry 0 is the head marker's summary; only the entries after it are candidates.
    Json v = view({{user(text(40))}, {user(text(40))}, {assistant(text(40))}, {user(text(40))}, {assistant(text(40))}}, head);
    EXPECT_EQ(m_planner.selectCut(v, 15), std::optional<std::size_t>(3));
    // Keeping everything after the marker leaves nothing but the marker to summarize.
    EXPECT_FALSE(m_planner.selectCut(view({{user(text(40))}, {assistant(text(40))}, {user(text(40))}}, head), 15).has_value());
    // Entries before the cut contribute nothing: no compaction.
    Json empty = view({{}, {}, {user(text(80))}});
    EXPECT_FALSE(m_planner.selectCut(empty, 5).has_value());
}

TEST_F(CompactionPlannerTest, SummarizedMessagesTakeTheEntriesBeforeTheCutInContextOrder) {
    Json v = view({{user("one")}, {assistant("two", Json::array({call("a")}))}, {result("a", "res")}, {user("three")}});
    Json messages = m_planner.summarizedMessages(v, 3);
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[2].at("toolCallId"), "a");
}

TEST_F(CompactionPlannerTest, EstimateUsesTheNewestMeasuredAssistantPlusTheTrailingMessages) {
    Json v = view({{user(text(400))}, {assistant("a", Json::array(), 500)}, {user(text(40))}});
    // 500 from the assistant's usage, plus the 40-char user message after it, plus extra.
    EXPECT_EQ(m_planner.estimateContext(v, Json::array()), 510);
    EXPECT_EQ(m_planner.estimateContext(v, Json::array({user(text(8))})), 512);
    // Without a measured assistant every message is estimated.
    Json unmeasured = view({{user(text(400))}, {user(text(40))}});
    EXPECT_EQ(m_planner.estimateContext(unmeasured, Json::array()), 110);
}

TEST_F(CompactionPlannerTest, EstimateIgnoresAssistantsBeforeTheHeadMarker) {
    Json head = Json::object({{"id", 11}, {"kind", "pi.compaction"}, {"head", 12}});
    // Entry ids start at 10, so the assistant (id 11) is at or before the head marker and does not count.
    Json v = view({{user(text(40))}, {assistant("a", Json::array(), 9000)}, {user(text(40))}}, head);
    // Every message is estimated (10 + 1 + 10), the assistant's usage is ignored.
    EXPECT_EQ(m_planner.estimateContext(v, Json::array()), 21);
}

TEST_F(CompactionPlannerTest, SerializationReadsLikeATranscriptAndTruncatesToolResults) {
    Json thinking = Json::object({{"type", "thinking"}, {"thinking", "hmm"}});
    Json message = assistant("done", Json::array({call("a")}));
    message["content"].insert(message["content"].begin(), thinking);
    Json messages = Json::array({user("question"), Json::object({{"role", "system"}, {"content", "ignored"}}), message,
                                 result("a", text(2500))});
    const std::string serialized = m_planner.serializeConversation(messages);
    EXPECT_NE(serialized.find("[User]: question"), std::string::npos);
    EXPECT_EQ(serialized.find("ignored"), std::string::npos);
    EXPECT_NE(serialized.find("[Assistant thinking]: hmm"), std::string::npos);
    EXPECT_NE(serialized.find("[Assistant]: done"), std::string::npos);
    EXPECT_NE(serialized.find("[Assistant tool calls]: read(path=\"a.txt\")"), std::string::npos);
    EXPECT_NE(serialized.find("[... 500 more characters truncated]"), std::string::npos);
    const std::string prompt = m_planner.summaryPrompt(messages, std::string("focus on files"));
    EXPECT_EQ(prompt.rfind("<conversation>\n", 0), 0u);
    EXPECT_NE(prompt.find("## Goal"), std::string::npos);
    EXPECT_NE(prompt.find("\n\nAdditional focus: focus on files"), std::string::npos);
}

TEST_F(CompactionPlannerTest, SummaryTextRequiresACleanStopWithTextAndNoToolCall) {
    EXPECT_EQ(m_planner.summaryText(assistant("  ## Goal\nx  ")), std::optional<std::string>("## Goal\nx"));
    EXPECT_FALSE(m_planner.summaryText(assistant("  ")).has_value());
    EXPECT_FALSE(m_planner.summaryText(assistant("text", Json::array({call("a")}))).has_value());
    Json length = assistant("cut");
    length["stopReason"] = "length";
    EXPECT_FALSE(m_planner.summaryText(length).has_value());
    EXPECT_EQ(m_planner.summaryFailure(length), "Summarization hit the token limit; the summary is incomplete");
    Json error = assistant("x");
    error["stopReason"] = "error";
    error["errorMessage"] = "boom";
    EXPECT_EQ(m_planner.summaryFailure(error), "Summarization failed: boom");
    EXPECT_EQ(m_planner.summaryFailure(assistant("text", Json::array({call("a")}))), "Summarization attempted to call a tool");
    EXPECT_EQ(m_planner.summaryFailure(assistant(" ")), "Summarization produced no text");
}

TEST_F(CompactionPlannerTest, CreateCompactionRegistersTheTaskAndItsLiveStatus) {
    auto storage = std::make_shared<MemoryStorage>();
    DurableSession session(storage);
    BuiltinDocuments documents;
    std::int64_t conversationId = 0;
    std::int64_t manual = 0;
    std::int64_t automatic = 0;
    std::int64_t blocking = 0;
    ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
        if (!created) {
            return std::unexpected(created.error());
        }
        conversationId = created->at("id").get<std::int64_t>();
        auto owner = tx.createTask("pi.generation", 1, Json::object(), Json::object({{"phase", "prepare"}, {"attempt", 1}}),
                                   [&] {
                                       TaskOptions options;
                                       options.ownership = Json::object({{"kind", "conversation"}});
                                       options.conversationId = conversationId;
                                       return options;
                                   }());
        if (!owner) {
            return std::unexpected(owner.error());
        }
        auto first = m_planner.createCompaction(tx, conversationId, Json::object({{"reason", "manual"}}));
        auto second = m_planner.createCompaction(tx, conversationId, Json::object({{"reason", "threshold"}}));
        auto third = m_planner.createCompaction(tx, conversationId, Json::object({{"reason", "overflow"}}), *owner);
        if (!first || !second || !third) {
            return std::unexpected(Error{"x", "create failed"});
        }
        manual = *first;
        automatic = *second;
        blocking = *third;
        return {};
    }).has_value());
    auto manualTask = storage->task(manual);
    auto automaticTask = storage->task(automatic);
    auto blockingTask = storage->task(blocking);
    ASSERT_TRUE(manualTask->has_value() && automaticTask->has_value() && blockingTask->has_value());
    EXPECT_EQ((*manualTask)->at("background"), false);
    EXPECT_EQ((*automaticTask)->at("background"), true);
    EXPECT_EQ((*blockingTask)->at("background"), false);
    EXPECT_TRUE((*blockingTask)->contains("owner"));
    DocAddressArgs args;
    args.owner = conversationId;
    auto live = session.snapshot(documents.live(), args);
    ASSERT_TRUE(live.has_value() && live->has_value());
    const Json& statuses = (*live)->at("compactions");
    ASSERT_EQ(statuses.size(), 3u);
    EXPECT_EQ(statuses[0].at("blocking"), false);
    EXPECT_EQ(statuses[2].at("blocking"), true);
    EXPECT_EQ(statuses[2].at("reason"), "overflow");
    EXPECT_EQ(statuses[0].at("attempt"), 1);
}
