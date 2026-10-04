#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.hook_bus;
import pi.support.plugin_session_events;

class PluginSessionEventsTest : public testing::Test {
protected:
    void on(const std::string& event, const std::function<Json(const Json&)>& handler) {
        m_bus.subscribe(event, [handler](const std::string&, const Json& payload) -> Result<Json> { return handler(payload); });
    }

    CompactionPreparation preparation() {
        CompactionPreparation prepared;
        prepared.firstKeptEntryId = "e5";
        prepared.tokensBefore = 1234;
        prepared.previousSummary = "earlier";
        prepared.fileOps.read.insert("a.txt");
        return prepared;
    }

    SessionEntry entry(const std::string& id) {
        SessionEntry out;
        out.type = "message";
        out.id = id;
        out.body = Json{{"type", "message"}, {"id", id}};
        return out;
    }

    HookBus m_bus;
    PluginSessionEvents m_events{m_bus};
};

TEST_F(PluginSessionEventsTest, WithoutSubscribersNothingChanges) {
    const InputOutcome input = m_events.input("hi", {}, "rpc", "");
    EXPECT_FALSE(input.handled);
    EXPECT_EQ(input.text, "hi");
    const AgentStartOutcome start = m_events.beforeAgentStart("hi", {}, "prompt");
    EXPECT_TRUE(start.messages.empty());
    EXPECT_FALSE(start.systemPrompt.has_value());
    EXPECT_FALSE(m_events.beforeProviderRequest(Json::object()).has_value());
    EXPECT_FALSE(m_events.beforeCompact(preparation(), {}, std::nullopt, "manual", false).cancel);
    EXPECT_FALSE(m_events.beforeTree(TreePreparation{}).cancel);
}

TEST_F(PluginSessionEventsTest, InputTransformsChainAndHandledEndsTheChain) {
    Json first;
    on("input", [&first](const Json& payload) {
        first = payload;
        return Json{{"action", "transform"}, {"text", "HI"}};
    });
    std::string second;
    on("input", [&second](const Json& payload) {
        second = payload["text"].get<std::string>();
        return Json{{"action", "continue"}};
    });
    ImageContent image{"AAAA", "image/png"};
    const InputOutcome outcome = m_events.input("hi", {image}, "rpc", "steer");
    EXPECT_FALSE(outcome.handled);
    EXPECT_EQ(outcome.text, "HI");
    ASSERT_EQ(outcome.images.size(), 1u);
    EXPECT_EQ(outcome.images[0].data, "AAAA");
    EXPECT_EQ(first["source"], "rpc");
    EXPECT_EQ(first["streamingBehavior"], "steer");
    EXPECT_EQ(first["images"][0]["mimeType"], "image/png");
    EXPECT_EQ(second, "HI");

    on("input", [](const Json&) { return Json{{"action", "handled"}}; });
    on("input", [](const Json&) { return Json{{"action", "transform"}, {"text", "late"}}; });
    const InputOutcome handled = m_events.input("hi", {}, "rpc", "");
    EXPECT_TRUE(handled.handled);
    EXPECT_EQ(handled.text, "HI");
}

TEST_F(PluginSessionEventsTest, InputTransformCanReplaceTheImages) {
    on("input", [](const Json&) {
        return Json{{"action", "transform"}, {"text", "x"}, {"images", Json::array({Json{{"type", "image"}, {"data", "BBBB"}, {"mimeType", "image/jpeg"}}})}};
    });
    const InputOutcome outcome = m_events.input("hi", {ImageContent{"AAAA", "image/png"}}, "rpc", "");
    ASSERT_EQ(outcome.images.size(), 1u);
    EXPECT_EQ(outcome.images[0].mimeType, "image/jpeg");
}

TEST_F(PluginSessionEventsTest, BeforeAgentStartCollectsMessagesAndTheLastSystemPrompt) {
    on("before_agent_start", [](const Json& payload) {
        EXPECT_EQ(payload["prompt"], "do it");
        EXPECT_EQ(payload["systemPrompt"], "base");
        return Json{{"message", Json{{"customType", "note"}, {"content", "remember"}}}, {"systemPrompt", "first"}};
    });
    std::string seen;
    on("before_agent_start", [&seen](const Json& payload) {
        seen = payload["systemPrompt"].get<std::string>();
        return Json{{"systemPrompt", "second"}};
    });
    const AgentStartOutcome outcome = m_events.beforeAgentStart("do it", {}, "base");
    ASSERT_EQ(outcome.messages.size(), 1u);
    EXPECT_EQ(outcome.messages[0]["customType"], "note");
    EXPECT_EQ(seen, "first");
    ASSERT_TRUE(outcome.systemPrompt.has_value());
    EXPECT_EQ(*outcome.systemPrompt, "second");
}

TEST_F(PluginSessionEventsTest, ProviderRequestPayloadsAreReplacedInOrder) {
    on("before_provider_request", [](const Json& payload) {
        Json out = payload["payload"];
        out["temperature"] = 0;
        return out;
    });
    on("before_provider_request", [](const Json&) { return Json(); });
    on("before_provider_request", [](const Json& payload) {
        Json out = payload["payload"];
        out["marker"] = true;
        return out;
    });
    const auto replaced = m_events.beforeProviderRequest(Json{{"model", "m"}});
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ((*replaced)["model"], "m");
    EXPECT_EQ((*replaced)["temperature"], 0);
    EXPECT_EQ((*replaced)["marker"], true);
}

TEST_F(PluginSessionEventsTest, BeforeCompactCanSupplyTheCompaction) {
    Json seen;
    on("session_before_compact", [&seen](const Json& payload) {
        seen = payload;
        return Json{{"compaction", Json{{"summary", "mine"}, {"firstKeptEntryId", "e9"}, {"tokensBefore", 77}, {"details", Json{{"k", 1}}}}}};
    });
    const BeforeCompactOutcome outcome = m_events.beforeCompact(preparation(), {entry("e1"), entry("e2")}, std::string("focus"), "threshold", false);
    EXPECT_FALSE(outcome.cancel);
    ASSERT_TRUE(outcome.compaction.has_value());
    EXPECT_EQ(outcome.compaction->summary, "mine");
    EXPECT_EQ(outcome.compaction->firstKeptEntryId, "e9");
    EXPECT_EQ(outcome.compaction->tokensBefore, 77);
    EXPECT_EQ(outcome.compaction->details["k"], 1);
    EXPECT_EQ(seen["reason"], "threshold");
    EXPECT_EQ(seen["customInstructions"], "focus");
    EXPECT_EQ(seen["preparation"]["firstKeptEntryId"], "e5");
    EXPECT_EQ(seen["preparation"]["previousSummary"], "earlier");
    EXPECT_EQ(seen["preparation"]["fileOps"]["read"][0], "a.txt");
    EXPECT_EQ(seen["branchEntries"].size(), 2u);
}

TEST_F(PluginSessionEventsTest, BeforeCompactCancelWinsOverEarlierCompactions) {
    on("session_before_compact", [](const Json&) {
        return Json{{"compaction", Json{{"summary", "mine"}, {"firstKeptEntryId", "e9"}}}};
    });
    on("session_before_compact", [](const Json&) { return Json{{"cancel", true}}; });
    const BeforeCompactOutcome outcome = m_events.beforeCompact(preparation(), {}, std::nullopt, "manual", false);
    EXPECT_TRUE(outcome.cancel);
    EXPECT_FALSE(outcome.compaction.has_value());
}

TEST_F(PluginSessionEventsTest, CompactionObservationsCarryTheirData) {
    Json compacted;
    Json failed;
    on("session_compact", [&compacted](const Json& payload) {
        compacted = payload;
        return Json();
    });
    on("session_compact_failed", [&failed](const Json& payload) {
        failed = payload;
        return Json();
    });
    m_events.compacted(entry("c1"), true, "overflow", true);
    m_events.compactFailed("manual", std::string("boom"), false, false, true);
    EXPECT_EQ(compacted["compactionEntry"]["id"], "c1");
    EXPECT_EQ(compacted["fromExtension"], true);
    EXPECT_EQ(compacted["reason"], "overflow");
    EXPECT_EQ(failed["errorMessage"], "boom");
    EXPECT_EQ(failed["aborted"], false);
    EXPECT_EQ(failed["fromExtension"], true);
}

TEST_F(PluginSessionEventsTest, BeforeTreeOverridesSummarizationOptions) {
    Json seen;
    on("session_before_tree", [&seen](const Json& payload) {
        seen = payload;
        return Json{{"customInstructions", "short"}, {"label", "L"}, {"replaceInstructions", true}};
    });
    TreePreparation prepared;
    prepared.targetId = "t1";
    prepared.oldLeafId = "o1";
    prepared.entriesToSummarize = {entry("e1")};
    prepared.userWantsSummary = true;
    const BeforeTreeOutcome outcome = m_events.beforeTree(prepared);
    EXPECT_FALSE(outcome.cancel);
    EXPECT_EQ(outcome.customInstructions, std::optional<std::string>("short"));
    EXPECT_EQ(outcome.label, std::optional<std::string>("L"));
    EXPECT_EQ(outcome.replaceInstructions, std::optional<bool>(true));
    EXPECT_FALSE(outcome.summary.has_value());
    EXPECT_EQ(seen["preparation"]["targetId"], "t1");
    EXPECT_EQ(seen["preparation"]["oldLeafId"], "o1");
    EXPECT_TRUE(seen["preparation"]["commonAncestorId"].is_null());
    EXPECT_EQ(seen["preparation"]["userWantsSummary"], true);
}

TEST_F(PluginSessionEventsTest, BeforeTreeCanSupplyTheSummaryOrCancel) {
    on("session_before_tree", [](const Json&) {
        return Json{{"summary", Json{{"summary", "own"}, {"details", Json{{"x", 1}}}}}};
    });
    BeforeTreeOutcome outcome = m_events.beforeTree(TreePreparation{});
    ASSERT_TRUE(outcome.summary.has_value());
    EXPECT_EQ(*outcome.summary, "own");
    EXPECT_EQ(outcome.details["x"], 1);
    on("session_before_tree", [](const Json&) { return Json{{"cancel", true}}; });
    outcome = m_events.beforeTree(TreePreparation{});
    EXPECT_TRUE(outcome.cancel);
    EXPECT_FALSE(outcome.summary.has_value());
}

TEST_F(PluginSessionEventsTest, TreeNavigationIsObservable) {
    Json seen;
    on("session_tree", [&seen](const Json& payload) {
        seen = payload;
        return Json();
    });
    m_events.treeNavigated(std::string("n1"), std::nullopt, entry("s1"), true);
    EXPECT_EQ(seen["newLeafId"], "n1");
    EXPECT_TRUE(seen["oldLeafId"].is_null());
    EXPECT_EQ(seen["summaryEntry"]["id"], "s1");
    EXPECT_EQ(seen["fromExtension"], true);
    m_events.treeNavigated(std::nullopt, std::string("o"), std::nullopt, false);
    EXPECT_FALSE(seen.contains("summaryEntry"));
}
