#include <gtest/gtest.h>

import std;
import pi.support.agent_event_translator;

class AgentEventTranslatorTest : public ::testing::Test {
protected:
    Json view(const Json& live, const Json& entries = Json::array(), const Json& extraDocs = Json::object()) {
        Json docs = Json::object({{"pi.live", live}});
        for (const auto& item : extraDocs.items()) {
            docs[item.key()] = item.value();
        }
        return Json::object({{"conversation", Json::object({{"id", 1}})}, {"entries", entries}, {"docs", docs}});
    }

    Json publication(const std::vector<Json>& changes) {
        return Json::object({{"seq", 1}, {"changes", Json(changes)}});
    }

    std::vector<Json> translate(const Json& before, const Json& after, const std::vector<Json>& changes = {}) {
        return m_translator.translate(1, before, after, publication(changes), m_held);
    }

    std::string types(const std::vector<Json>& events) {
        std::string out;
        for (const Json& event : events) {
            out += (out.empty() ? "" : " ") + event.at("type").get<std::string>();
        }
        return out;
    }

    Json entry(std::int64_t id, const Json& message) {
        return Json::object({{"id", id}, {"conversationId", 1}, {"kind", "m"}, {"model", Json::array({message})}});
    }

    Json entryChange(const Json& record) {
        return Json::object({{"type", "entry"}, {"value", record}});
    }

    Json assistant(const Json& content) {
        return Json::object({{"role", "assistant"}, {"content", content}, {"usage", Json::object({{"input", 1}})}});
    }

    Json text(const std::string& value) {
        return Json::object({{"type", "text"}, {"text", value}});
    }

    Json generation(const Json& message) {
        return Json::object({{"generation", Json::object({{"attempt", 1}, {"message", message}})}});
    }

    AgentEventTranslator m_translator;
    std::set<std::int64_t> m_held;
};

TEST_F(AgentEventTranslatorTest, TheSnapshotDescribesTheViewAndDefaultsMissingDocuments) {
    Json live = Json::object({{"run", Json::object({{"taskId", 3}, {"inputs", Json::array({7})}})}, {"tools", Json::array({Json::object({{"callId", "c"}, {"status", "running"}})})}});
    const Json snapshot = m_translator.snapshot(view(live, Json::array({entry(2, Json::object({{"role", "user"}}))}), Json::object({{"pi.inbox", Json::object({{"items", Json::array({Json::object({{"id", 9}, {"mode", "steer"}, {"x", 1}})})}})}})));
    EXPECT_EQ(snapshot.at("type"), "snapshot");
    EXPECT_EQ(snapshot.at("run"), (Json{{"inputs", Json::array({7})}}));
    EXPECT_EQ(snapshot.at("tools").size(), 1u);
    EXPECT_EQ(snapshot.at("inbox"), Json::array({Json{{"id", 9}, {"mode", "steer"}}}));
    EXPECT_EQ(snapshot.at("agent"), Json::object());
    EXPECT_EQ(snapshot.at("usage"), (Json{{"models", Json::object()}, {"tools", Json::object()}}));
    EXPECT_FALSE(snapshot.contains("generation"));
    EXPECT_EQ(snapshot.at("entries").size(), 1u);
}

TEST_F(AgentEventTranslatorTest, AFirstPartialStartsAMessageAndLaterChangesAreDeltas) {
    const Json none = view(Json::object());
    const Json first = view(generation(assistant(Json::array({text("he")}))));
    EXPECT_EQ(types(translate(none, first)), "message_start");
    const Json second = view(generation(assistant(Json::array({text("hello"), Json::object({{"type", "thinking"}, {"thinking", "t"}})}))));
    const std::vector<Json> update = translate(first, second);
    ASSERT_EQ(update.size(), 1u);
    EXPECT_EQ(update[0].at("type"), "message_update");
    EXPECT_EQ(update[0].at("usage"), (Json{{"input", 1}}));
    const Json changes = update[0].at("changes");
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0], (Json{{"type", "text_delta"}, {"contentIndex", 0}, {"delta", "llo"}}));
    EXPECT_EQ(changes[1].at("type"), "thinking_start");
    EXPECT_EQ(changes[1].at("contentIndex"), 1);
}

TEST_F(AgentEventTranslatorTest, ToolCallArgumentsAndOtherBlockChangesTravelAsTheyChange) {
    const Json call = Json::object({{"type", "toolCall"}, {"id", "c"}, {"name", "x"}, {"arguments", Json::object({{"path", "a"}})}});
    const Json before = view(generation(assistant(Json::array({call}))));
    Json grown = call;
    grown["arguments"]["path"] = "abc";
    const std::vector<Json> delta = translate(before, view(generation(assistant(Json::array({grown})))));
    ASSERT_EQ(delta.size(), 1u);
    const Json change = delta[0].at("changes")[0];
    EXPECT_EQ(change.at("type"), "toolcall_delta");
    EXPECT_EQ(change.at("path"), Json::array({"path"}));
    EXPECT_EQ(change.at("delta"), "bc");
    Json renamed = call;
    renamed["name"] = "y";
    const std::vector<Json> block = translate(before, view(generation(assistant(Json::array({renamed})))));
    ASSERT_EQ(block.size(), 1u);
    EXPECT_EQ(block[0].at("changes")[0].at("type"), "block");
    EXPECT_EQ(block[0].at("changes")[0].at("block").at("name"), "y");
}

TEST_F(AgentEventTranslatorTest, AUsageOnlyChangeSendsAnUpdateWithoutChanges) {
    const Json before = view(generation(assistant(Json::array({text("a")}))));
    Json message = assistant(Json::array({text("a")}));
    message["usage"] = Json::object({{"input", 5}});
    const std::vector<Json> events = translate(before, view(generation(message)));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].at("changes").empty());
    EXPECT_EQ(events[0].at("usage"), (Json{{"input", 5}}));
}

TEST_F(AgentEventTranslatorTest, ToolsStartUpdateAndEndAroundTheirResultEntry) {
    const Json running = Json::object({{"callId", "c1"}, {"name", "read"}, {"status", "running"}, {"taskId", 11}});
    const Json task = Json::object({{"type", "task"}, {"value", Json::object({{"id", 11}, {"conversationId", 1}, {"kind", "pi.tool"}, {"state", Json::object({{"status", "running"}, {"checkpoint", Json::object({{"arguments", Json::object({{"path", "a"}})}})}})}})}});
    const std::vector<Json> started = translate(view(Json::object()), view(Json::object({{"tools", Json::array({running})}})), {task});
    ASSERT_EQ(started.size(), 1u);
    EXPECT_EQ(started[0], (Json{{"type", "tool_execution_start"}, {"toolCallId", "c1"}, {"toolName", "read"}, {"args", Json{{"path", "a"}}}}));

    Json withOutput = running;
    withOutput["output"] = "ab";
    Json grown = running;
    grown["output"] = "abc";
    const std::vector<Json> appended = translate(view(Json::object({{"tools", Json::array({withOutput})}})), view(Json::object({{"tools", Json::array({grown})}})));
    ASSERT_EQ(appended.size(), 1u);
    EXPECT_EQ(appended[0].at("type"), "tool_execution_update");
    EXPECT_EQ(appended[0].at("output"), (Json{{"append", "c"}}));

    Json details = grown;
    details["details"] = Json::object({{"n", 1}});
    const std::vector<Json> detailed = translate(view(Json::object({{"tools", Json::array({grown})}})), view(Json::object({{"tools", Json::array({details})}})));
    ASSERT_EQ(detailed.size(), 1u);
    EXPECT_EQ(detailed[0].at("details"), (Json{{"n", 1}}));
    EXPECT_FALSE(detailed[0].contains("output"));
    const std::vector<Json> cleared = translate(view(Json::object({{"tools", Json::array({details})}})), view(Json::object({{"tools", Json::array({grown})}})));
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_TRUE(cleared[0].at("details").is_null());

    Json done = running;
    done["status"] = "done";
    done["entry"] = 20;
    const Json result = entry(20, Json::object({{"role", "toolResult"}, {"toolCallId", "c1"}}));
    const std::vector<Json> ended = translate(view(Json::object({{"tools", Json::array({running})}})), view(Json::object({{"tools", Json::array({done})}}), Json::array({result})), {entryChange(result)});
    EXPECT_EQ(types(ended), "tool_execution_end message_start message_end");
    EXPECT_EQ(ended[0].at("entry").at("id"), 20);
}

TEST_F(AgentEventTranslatorTest, AToolSlotThatVanishesUnfinishedEndsWithTheResultAppendedWithIt) {
    const Json running = Json::object({{"callId", "c1"}, {"name", "read"}, {"status", "running"}});
    const Json result = entry(21, Json::object({{"role", "toolResult"}, {"toolCallId", "c1"}}));
    const std::vector<Json> events = translate(view(Json::object({{"tools", Json::array({running})}})), view(Json::object(), Json::array({result})), {entryChange(result)});
    EXPECT_EQ(types(events), "tool_execution_end message_start message_end");
    // Without a result entry the end follows the entries and carries none.
    const std::vector<Json> bare = translate(view(Json::object({{"tools", Json::array({running})}})), view(Json::object()));
    ASSERT_EQ(bare.size(), 1u);
    EXPECT_EQ(bare[0].at("type"), "tool_execution_end");
    EXPECT_FALSE(bare[0].contains("entry"));
}

TEST_F(AgentEventTranslatorTest, AStreamedAnswerOnlyEndsAndEntriesWithoutMessagesAreAppended) {
    const Json before = view(generation(assistant(Json::array({text("hi")}))));
    const Json answer = entry(5, assistant(Json::array({text("hi")})));
    const Json marker = Json::object({{"id", 6}, {"conversationId", 1}, {"kind", "pi.custom"}});
    const std::vector<Json> events = translate(before, view(Json::object(), Json::array({answer, marker})), {entryChange(answer), entryChange(marker)});
    EXPECT_EQ(types(events), "message_end entry_appended");
}

TEST_F(AgentEventTranslatorTest, RetryAndDeferredStateAreEvents) {
    const Json plain = view(Json::object({{"generation", Json::object({{"attempt", 2}})}}));
    const Json retrying = view(Json::object({{"generation", Json::object({{"attempt", 2}, {"retry", Json::object({{"at", 99}, {"error", "boom"}})}})}}));
    const std::vector<Json> start = translate(plain, retrying);
    ASSERT_EQ(start.size(), 1u);
    EXPECT_EQ(start[0], (Json{{"type", "auto_retry_start"}, {"attempt", 2}, {"at", 99}, {"errorMessage", "boom"}}));
    const std::vector<Json> end = translate(retrying, plain);
    ASSERT_EQ(end.size(), 1u);
    EXPECT_EQ(end[0], (Json{{"type", "auto_retry_end"}, {"attempt", 2}}));
    const Json polling = view(Json::object({{"generation", Json::object({{"attempt", 1}, {"deferred", Json::object({{"pollAt", 5}})}})}}));
    const Json later = view(Json::object({{"generation", Json::object({{"attempt", 1}, {"deferred", Json::object({{"pollAt", 9}})}})}}));
    EXPECT_EQ(translate(view(Json::object()), polling)[0], (Json{{"type", "deferred_poll"}, {"pollAt", 5}}));
    EXPECT_EQ(translate(polling, later)[0], (Json{{"type", "deferred_poll"}, {"pollAt", 9}}));
}

TEST_F(AgentEventTranslatorTest, RunsAndTurnsStartAndEndInTheOrderOfTheSpec) {
    const Json generationTask = Json::object({{"type", "task"}, {"value", Json::object({{"id", 3}, {"conversationId", 1}, {"kind", "pi.generation"}, {"state", Json::object({{"status", "pending"}, {"checkpoint", Json::object({{"phase", "prepare"}})}})}})}});
    const Json run = Json::object({{"run", Json::object({{"taskId", 3}, {"inputs", Json::array({7})}})}});
    const Json submission = Json::object({{"type", "submission"}, {"value", Json::object({{"id", 7}, {"conversationId", 1}, {"status", "placed"}})}});
    const std::vector<Json> began = translate(view(Json::object()), view(run), {generationTask, submission});
    EXPECT_EQ(types(began), "submission run_start turn_start");

    auto finished = [&](const std::string& status) {
        Json state = Json::object({{"status", status}});
        if (status == "terminal") {
            state["outcome"] = Json::object({{"status", "completed"}});
        }
        return Json::object({{"type", "task"}, {"value", Json::object({{"id", 3}, {"conversationId", 1}, {"kind", "pi.generation"}, {"state", state}})}});
    };
    const std::vector<Json> held = translate(view(run), view(run), {finished("completing")});
    EXPECT_EQ(types(held), "turn_end");
    EXPECT_TRUE(m_held.contains(3));
    // The terminal commit of a held generation ends no second turn, and the run ends with its removal.
    const std::vector<Json> ended = translate(view(run), view(Json::object()), {finished("terminal")});
    EXPECT_EQ(types(ended), "run_end");
    EXPECT_FALSE(m_held.contains(3));
    // A generation that is never held ends its turn at the terminal commit.
    EXPECT_EQ(types(translate(view(run), view(run), {finished("terminal")})), "turn_end");
}

TEST_F(AgentEventTranslatorTest, FailedAndOrphanedTasksAreReported) {
    auto settled = [&](std::int64_t id, const Json& outcome) {
        return Json::object({{"type", "task"}, {"value", Json::object({{"id", id}, {"conversationId", 1}, {"kind", "pi.tool"}, {"state", Json::object({{"status", "terminal"}, {"outcome", outcome}})}})}});
    };
    const std::vector<Json> events = translate(view(Json::object()), view(Json::object()),
                                              {settled(4, Json{{"status", "faulted"}, {"error", Json{{"message", "bad"}}}}), settled(5, Json{{"status", "orphaned"}, {"reason", "gone"}}), settled(6, Json{{"status", "completed"}})});
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], (Json{{"type", "task_failed"}, {"taskId", 4}, {"kind", "pi.tool"}, {"message", "bad"}}));
    EXPECT_EQ(events[1], (Json{{"type", "task_failed"}, {"taskId", 5}, {"kind", "pi.tool"}, {"message", "gone"}}));
}

TEST_F(AgentEventTranslatorTest, CompactionsAppearAndDisappear) {
    const Json status = Json::object({{"taskId", 8}, {"reason", "threshold"}, {"blocking", true}});
    const Json compacting = view(Json::object({{"compactions", Json::array({status})}}));
    EXPECT_EQ(translate(view(Json::object()), compacting)[0], (Json{{"type", "compaction_start"}, {"taskId", 8}, {"reason", "threshold"}, {"blocking", true}}));
    EXPECT_EQ(translate(compacting, view(Json::object()))[0], (Json{{"type", "compaction_end"}, {"taskId", 8}, {"reason", "threshold"}}));
}

TEST_F(AgentEventTranslatorTest, DocumentChangesAreEventsAndRetiredOnesReadAsTheirInitialValue) {
    const Json inbox = Json::object({{"items", Json::array({Json::object({{"id", 3}, {"mode", "followUp"}})})}});
    const std::vector<Json> queued = translate(view(Json::object()), view(Json::object(), Json::array(), Json::object({{"pi.inbox", inbox}})));
    ASSERT_EQ(queued.size(), 1u);
    EXPECT_EQ(queued[0], (Json{{"type", "inbox_update"}, {"items", Json::array({Json{{"id", 3}, {"mode", "followUp"}}})}}));
    const std::vector<Json> agent = translate(view(Json::object(), Json::array(), Json::object({{"pi.agent", Json{{"cwd", "/a"}}}})), view(Json::object()));
    ASSERT_EQ(agent.size(), 1u);
    EXPECT_EQ(agent[0], (Json{{"type", "agent_changed"}, {"agent", Json::object()}}));
    const std::vector<Json> usage = translate(view(Json::object()), view(Json::object(), Json::array(), Json::object({{"pi.usage", Json{{"models", Json::object()}, {"tools", Json::object({{"read", 1}})}}}})));
    ASSERT_EQ(usage.size(), 1u);
    EXPECT_EQ(usage[0].at("type"), "usage_changed");
}

TEST_F(AgentEventTranslatorTest, ChangesOfOtherConversationsAndUnrelatedPublicationsYieldNothing) {
    const Json other = Json::object({{"type", "entry"}, {"value", Json::object({{"id", 9}, {"conversationId", 2}, {"kind", "m"}})}});
    EXPECT_TRUE(translate(view(Json::object()), view(Json::object()), {other}).empty());
    EXPECT_TRUE(translate(view(Json::object()), view(Json::object())).empty());
    const Json retired = Json::object({{"type", "document"}, {"value", nullptr}, {"record", Json::object()}});
    EXPECT_TRUE(translate(view(Json::object()), view(Json::object()), {retired}).empty());
}
