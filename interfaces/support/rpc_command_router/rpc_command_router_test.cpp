#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.agent_session_runtime;
import pi.session.session_store;
import pi.session_manager_factory;
import pi.support.rpc_command_router;
import pi.testing.fake_model_runtime;
import pi.testing.session_harness;
import pi.testing.session_runtime_factory;

class RpcCommandRouterTest : public testing::Test {
protected:
    RpcCommandRouterTest() {
        m_harness.files().createDirectories("/tmp");
        SessionRuntimeRequest request;
        request.cwd = "/tmp";
        request.agentDir = "/agent";
        auto manager = m_store.create("/tmp", std::nullopt, std::nullopt, std::nullopt);
        request.sessionManager = std::move(*manager);
        auto handle = m_factory.create(std::move(request));
        m_runtime = std::make_unique<AgentSessionRuntime>(m_factory, m_store, m_harness.files(), std::move(*handle));
        Model model;
        model.provider = "faux";
        model.id = "faux-1";
        model.reasoning = true;
        m_models.addModel(model);
        m_models.setAuthenticated("faux", true);
        m_router = std::make_unique<RpcCommandRouter>(*m_runtime, m_models, [this](const Json& json) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_out.push_back(json);
        });
        m_router->attach();
    }

    Json send(Json command) {
        const std::size_t before = outputs().size();
        m_router->handle(command);
        const auto all = outputs();
        for (std::size_t i = before; i < all.size(); ++i) {
            if (all[i].value("type", "") == "response") {
                return all[i];
            }
        }
        return Json();
    }

    std::vector<Json> outputs() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_out;
    }

    std::vector<std::string> eventTypes() {
        std::vector<std::string> out;
        for (const auto& json : outputs()) {
            out.push_back(json.value("type", ""));
        }
        return out;
    }

    SessionHarness m_harness;
    SessionManagerFactory m_managers{m_harness.files(), m_harness.clock(), m_harness.ids()};
    SessionStore m_store{"/agent", m_harness.files(), m_harness.clock(), m_harness.ids(), m_managers};
    SessionRuntimeFactory m_factory{m_harness.agents(), m_harness.provider(), m_harness.files(), m_harness.clock(),
                                    m_harness.ids(), m_harness.sleeper()};
    FakeModelRuntime m_models;
    std::unique_ptr<AgentSessionRuntime> m_runtime;
    std::unique_ptr<RpcCommandRouter> m_router;
    std::mutex m_mutex;
    std::vector<Json> m_out;
};

TEST_F(RpcCommandRouterTest, PromptAnswersFirstThenStreamsEventsUntilSettled) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello"));
    m_router->handle(Json{{"id", "p1"}, {"type", "prompt"}, {"message", "hi"}});
    const auto all = outputs();
    ASSERT_GE(all.size(), 4U);
    EXPECT_EQ(all[0], (Json{{"id", "p1"}, {"type", "response"}, {"command", "prompt"}, {"success", true},
                            {"data", Json{{"disposition", "started"}}}}));
    const auto types = eventTypes();
    EXPECT_EQ(types[1], "agent_start");
    EXPECT_NE(std::ranges::find(types, "message_update"), types.end());
    EXPECT_EQ(types.back(), "agent_settled");
    const Json state = send(Json{{"type", "get_state"}})["data"];
    EXPECT_EQ(state["isStreaming"], false);
    EXPECT_EQ(state["messageCount"], 3);
    EXPECT_EQ(state["thinkingLevel"], "off");
    EXPECT_EQ(state["steeringMode"], "one-at-a-time");
    EXPECT_EQ(state["model"]["id"], "faux-1");
}

TEST_F(RpcCommandRouterTest, PromptWhileStreamingWithoutBehaviorReportsAnError) {
    m_harness.provider().enqueue([this](const TranscriptContext&, const StreamOptions&, const Model&) {
        m_router->handle(Json{{"id", "p2"}, {"type", "prompt"}, {"message", "again"}});
        return m_harness.provider().textResponse("first");
    });
    m_router->handle(Json{{"id", "p1"}, {"type", "prompt"}, {"message", "hi"}});
    bool rejected = false;
    for (const auto& json : outputs()) {
        if (json.value("id", "") == "p2") {
            rejected = true;
            EXPECT_EQ(json["success"], false);
            EXPECT_NE(json["error"].get<std::string>().find("already processing"), std::string::npos);
        }
    }
    EXPECT_TRUE(rejected);
}

TEST_F(RpcCommandRouterTest, UnknownAndMalformedCommands) {
    const Json unknown = send(Json{{"id", "x"}, {"type", "nope"}});
    EXPECT_EQ(unknown["error"], "Unknown command: nope");
    EXPECT_EQ(unknown["command"], "nope");
    const Json parse = m_router->parseError("bad json");
    EXPECT_EQ(parse["command"], "parse");
    EXPECT_FALSE(parse.contains("id"));
}

TEST_F(RpcCommandRouterTest, ModelAndThinkingCommands) {
    EXPECT_EQ(send(Json{{"type", "get_available_models"}})["data"]["models"].size(), 1U);
    const Json found = send(Json{{"type", "set_model"}, {"provider", "faux"}, {"modelId", "faux-1"}});
    EXPECT_EQ(found["data"]["id"], "faux-1");
    const Json missing = send(Json{{"type", "set_model"}, {"provider", "faux"}, {"modelId", "none"}});
    EXPECT_EQ(missing["error"], "Model not found: faux/none");
    EXPECT_EQ(send(Json{{"type", "cycle_model"}})["data"], nullptr);
    EXPECT_EQ(send(Json{{"type", "set_thinking_level"}, {"level", "high"}})["success"], true);
    EXPECT_EQ(send(Json{{"type", "set_thinking_level"}, {"level", "wild"}})["success"], false);
    const Json levels = send(Json{{"type", "get_available_thinking_levels"}})["data"]["levels"];
    EXPECT_FALSE(levels.empty());
}

TEST_F(RpcCommandRouterTest, QueueSettingsAndSessionName) {
    EXPECT_EQ(send(Json{{"type", "set_steering_mode"}, {"mode", "all"}})["success"], true);
    EXPECT_EQ(send(Json{{"type", "get_state"}})["data"]["steeringMode"], "all");
    EXPECT_EQ(send(Json{{"type", "set_session_name"}, {"name", "   "}})["error"], "Session name cannot be empty");
    EXPECT_EQ(send(Json{{"type", "set_session_name"}, {"name", " My chat "}})["success"], true);
    EXPECT_EQ(send(Json{{"type", "get_state"}})["data"]["sessionName"], "My chat");
    const auto types = eventTypes();
    EXPECT_NE(std::ranges::find(types, "session_info_changed"), types.end());
    EXPECT_EQ(send(Json{{"type", "clear_queue"}})["data"], (Json{{"steering", Json::array()}, {"followUp", Json::array()}}));
}

TEST_F(RpcCommandRouterTest, SessionTreeCommandsAndForks) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("one"));
    m_router->handle(Json{{"type", "prompt"}, {"message", "first question"}});
    m_harness.provider().enqueue(m_harness.provider().textResponse("two"));
    m_router->handle(Json{{"type", "prompt"}, {"message", "second question"}});

    const Json messages = send(Json{{"type", "get_fork_messages"}})["data"]["messages"];
    ASSERT_EQ(messages.size(), 2U);
    const Json entries = send(Json{{"type", "get_entries"}})["data"];
    EXPECT_GE(entries["entries"].size(), 5U);
    EXPECT_FALSE(entries["leafId"].is_null());
    const std::string firstId = entries["entries"][0]["id"];
    const Json since = send(Json{{"type", "get_entries"}, {"since", firstId}})["data"];
    EXPECT_EQ(since["entries"].size(), entries["entries"].size() - 1);
    EXPECT_FALSE(send(Json{{"type", "get_entries"}, {"since", "nope"}})["success"].get<bool>());
    EXPECT_EQ(send(Json{{"type", "get_tree"}})["data"]["tree"].size(), 1U);

    const Json forked = send(Json{{"type", "fork"}, {"entryId", messages[1]["entryId"]}});
    EXPECT_EQ(forked["data"]["text"], "second question");
    EXPECT_EQ(forked["data"]["cancelled"], false);
    EXPECT_EQ(send(Json{{"type", "get_last_assistant_text"}})["data"]["text"], "one");
    EXPECT_EQ(send(Json{{"type", "clone"}})["success"], true);
    EXPECT_FALSE(send(Json{{"type", "fork"}, {"entryId", "nope"}})["success"].get<bool>());
}

TEST_F(RpcCommandRouterTest, NewSessionKeepsStreamingEventsFromTheNewSession) {
    const std::string oldId = send(Json{{"type", "get_state"}})["data"]["sessionId"];
    EXPECT_EQ(send(Json{{"type", "new_session"}})["data"], (Json{{"cancelled", false}}));
    EXPECT_NE(send(Json{{"type", "get_state"}})["data"]["sessionId"], oldId);
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello"));
    m_router->handle(Json{{"type", "prompt"}, {"message", "hi"}});
    EXPECT_EQ(eventTypes().back(), "agent_settled");
}

TEST_F(RpcCommandRouterTest, BashCompactAndStatsCommands) {
    const Json bash = send(Json{{"id", "b1"}, {"type", "bash"}, {"command", "echo from-bash"}});
    EXPECT_EQ(bash["success"], true);
    EXPECT_EQ(bash["data"]["output"], "from-bash\n");
    const Json compact = send(Json{{"type", "compact"}});
    EXPECT_EQ(compact["success"], false);
    const Json stats = send(Json{{"type", "get_session_stats"}})["data"];
    EXPECT_EQ(stats["totalMessages"], 1);
    EXPECT_EQ(send(Json{{"type", "export_html"}})["success"], false);
    const Json bug = send(Json{{"type", "bug_report"}, {"delivery", "zip"}});
    EXPECT_EQ(bug["success"], false);
    EXPECT_EQ(bug["command"], "bug_report");
    EXPECT_EQ(bug["error"], "Bug reports are not available in this host");
    EXPECT_TRUE(send(Json{{"type", "get_commands"}})["data"]["commands"].empty());
    EXPECT_EQ(send(Json{{"type", "get_messages"}})["data"]["messages"].size(), 1U);
}

TEST_F(RpcCommandRouterTest, AbortReturnsOnceIdle) {
    EXPECT_EQ(send(Json{{"type", "abort"}})["success"], true);
    EXPECT_EQ(send(Json{{"type", "abort_retry"}})["success"], true);
    EXPECT_EQ(send(Json{{"type", "abort_bash"}})["success"], true);
}
