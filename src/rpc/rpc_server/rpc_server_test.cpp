#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.rpc.rpc_server;
import pi.session.agent_session_runtime;
import pi.session.session_store;
import pi.session_manager_factory;
import pi.testing.capturing_byte_output;
import pi.testing.fake_model_runtime;
import pi.testing.scripted_byte_input;
import pi.testing.session_harness;
import pi.testing.session_runtime_factory;

class RpcServerTest : public testing::Test {
protected:
    RpcServerTest() {
        m_harness.files().createDirectories("/tmp");
        SessionRuntimeRequest request;
        request.cwd = "/tmp";
        request.agentDir = "/agent";
        auto manager = m_store.create("/tmp", std::nullopt, std::nullopt, std::nullopt);
        request.sessionManager = std::move(*manager);
        auto handle = m_factory.create(std::move(request));
        m_runtime = std::make_unique<AgentSessionRuntime>(m_factory, m_store, m_harness.files(), std::move(*handle));
    }

    std::vector<Json> lines() {
        std::vector<Json> out;
        std::istringstream stream(m_output.text());
        for (std::string line; std::getline(stream, line);) {
            out.push_back(Json::parse(line));
        }
        return out;
    }

    int serve() {
        RpcServer server(*m_runtime, m_models, m_input, m_output, m_pool);
        return server.run();
    }

    SessionHarness m_harness;
    SessionManagerFactory m_managers{m_harness.files(), m_harness.clock(), m_harness.ids()};
    SessionStore m_store{"/agent", m_harness.files(), m_harness.clock(), m_harness.ids(), m_managers};
    SessionRuntimeFactory m_factory{m_harness.agents(), m_harness.provider(), m_harness.files(), m_harness.clock(),
                                    m_harness.ids(), m_harness.sleeper()};
    FakeModelRuntime m_models;
    ScriptedByteInput m_input;
    CapturingByteOutput m_output;
    ThreadPool m_pool{4};
    std::unique_ptr<AgentSessionRuntime> m_runtime;
};

TEST_F(RpcServerTest, ServesCommandsUntilEndOfInput) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello"));
    m_input.push("{\"id\":\"1\",\"type\":\"get_state\"}\n{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"hi\"}\n");
    m_input.close();
    EXPECT_EQ(serve(), 0);
    bool state = false;
    bool prompt = false;
    bool settled = false;
    for (const auto& line : lines()) {
        state = state || (line.value("id", "") == "1" && line["success"] == true);
        prompt = prompt || (line.value("id", "") == "2" && line["data"]["disposition"] == "started");
        settled = settled || line.value("type", "") == "agent_settled";
    }
    EXPECT_TRUE(state);
    EXPECT_TRUE(prompt);
    EXPECT_TRUE(settled);
}

TEST_F(RpcServerTest, MalformedLinesGetAParseErrorAndTheServerKeepsGoing) {
    m_input.push("not json\n{\"id\":\"3\",\"type\":\"get_last_assistant_text\"}\n");
    m_input.close();
    EXPECT_EQ(serve(), 0);
    const auto all = lines();
    ASSERT_GE(all.size(), 2U);
    bool sawParse = false;
    bool sawAnswer = false;
    for (const auto& line : all) {
        sawParse = sawParse || line.value("command", "") == "parse";
        sawAnswer = sawAnswer || line.value("id", "") == "3";
    }
    EXPECT_TRUE(sawParse);
    EXPECT_TRUE(sawAnswer);
}

TEST_F(RpcServerTest, CommandsSplitAcrossChunksAndUnterminatedLastLine) {
    m_input.push("{\"id\":\"a\",\"type\":\"get_");
    m_input.push("state\"}\n{\"id\":\"b\",\"type\":\"get_messages\"}");
    m_input.close();
    EXPECT_EQ(serve(), 0);
    std::set<std::string> ids;
    for (const auto& line : lines()) {
        if (line.contains("id")) {
            ids.insert(line["id"]);
        }
    }
    EXPECT_EQ(ids, (std::set<std::string>{"a", "b"}));
}

TEST_F(RpcServerTest, ExtensionUiResponsesAreIgnored) {
    m_input.push("{\"type\":\"extension_ui_response\",\"id\":\"x\",\"value\":\"y\"}\n");
    m_input.close();
    EXPECT_EQ(serve(), 0);
    EXPECT_TRUE(lines().empty());
}

TEST_F(RpcServerTest, AbortCanRunWhileAPromptIsStillStreaming) {
    std::atomic<bool> release{false};
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions&, const Model&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return m_harness.provider().textResponse("late");
    });
    m_input.push("{\"id\":\"p\",\"type\":\"prompt\",\"message\":\"go\"}\n");
    std::thread server([this] { serve(); });
    while (!m_runtime->session().isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    m_input.push("{\"id\":\"s\",\"type\":\"get_state\"}\n");
    bool answeredWhileRunning = false;
    for (int i = 0; i < 500 && !answeredWhileRunning; ++i) {
        for (const auto& line : lines()) {
            answeredWhileRunning = answeredWhileRunning ||
                                   (line.value("id", "") == "s" && line["data"]["isStreaming"] == true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    release = true;
    m_input.close();
    server.join();
    EXPECT_TRUE(answeredWhileRunning);
}
