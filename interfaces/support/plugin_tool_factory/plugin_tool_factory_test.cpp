#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pi_plugin.h"

import std;
import pi.support.plugin_tool_factory;

struct PluginToolRecorder {
    std::string toolCallId;
    std::string params;
    bool aborted = false;
    int releases = 0;
    std::string reply;
    bool sendUpdate = false;
    bool replyNothing = false;
};

class PluginToolFactoryTest : public testing::Test {
protected:
    static PiToolExecuteFn callback() {
        return [](void* userData, PiString toolCallId, PiString params, const PiAbort* abort, PiUpdateFn onUpdate,
                  void* updateContext) -> PiOwnedString {
            auto* recorder = static_cast<PluginToolRecorder*>(userData);
            recorder->toolCallId.assign(toolCallId.data, toolCallId.size);
            recorder->params.assign(params.data, params.size);
            recorder->aborted = reinterpret_cast<const AbortSignal*>(abort)->aborted();
            if (recorder->sendUpdate) {
                const std::string partial = R"({"content":[{"type":"text","text":"working"}],"details":{"step":1}})";
                onUpdate(updateContext, PiString{partial.data(), partial.size()});
            }
            if (recorder->replyNothing) {
                return PiOwnedString{nullptr, 0, nullptr};
            }
            char* copy = new char[recorder->reply.size()];
            std::memcpy(copy, recorder->reply.data(), recorder->reply.size());
            return PiOwnedString{copy, recorder->reply.size(), [](char* data, std::size_t) { delete[] data; }};
        };
    }

    Result<std::shared_ptr<ITool>> create(const Json& definition) {
        return m_factory.create(definition.dump(), callback(), &m_recorder);
    }

    Json minimal() {
        return Json{{"name", "hello"}, {"description", "Says hello"}, {"parameters", Json{{"type", "object"}}}};
    }

    Result<AgentToolResult> run(const std::string& reply, const ToolUpdateCallback& onUpdate = nullptr) {
        m_recorder.reply = reply;
        const auto tool = create(minimal());
        return (*tool)->execute("call-7", Json{{"who", "me"}}, m_signal, onUpdate);
    }

    PluginToolFactory m_factory;
    PluginToolRecorder m_recorder;
    std::shared_ptr<AbortSignal> m_signal = std::make_shared<AbortSignal>();
};

TEST_F(PluginToolFactoryTest, BuildsTheToolFromItsDefinition) {
    Json definition = minimal();
    definition["label"] = "Hello!";
    definition["promptSnippet"] = "greet people";
    definition["promptGuidelines"] = Json::array({"be nice", 3});
    definition["executionMode"] = "sequential";
    const auto tool = create(definition);
    ASSERT_TRUE(tool.has_value());
    EXPECT_EQ((*tool)->definition().name, "hello");
    EXPECT_EQ((*tool)->definition().description, "Says hello");
    EXPECT_EQ((*tool)->definition().parameters["type"], "object");
    EXPECT_EQ((*tool)->label(), "Hello!");
    EXPECT_EQ((*tool)->promptSnippet(), "greet people");
    EXPECT_EQ((*tool)->promptGuidelines(), std::vector<std::string>{"be nice"});
    EXPECT_EQ(*(*tool)->executionMode(), ToolExecutionMode::Sequential);
    const auto plain = create(minimal());
    EXPECT_EQ((*plain)->label(), "hello");
    EXPECT_FALSE((*plain)->executionMode().has_value());
}

TEST_F(PluginToolFactoryTest, RejectsBadDefinitions) {
    EXPECT_FALSE(m_factory.create("[]", callback(), nullptr).has_value());
    EXPECT_FALSE(m_factory.create("not json", callback(), nullptr).has_value());
    EXPECT_FALSE(create(Json{{"description", "x"}}).has_value());
    Json badParameters = minimal();
    badParameters["parameters"] = "x";
    EXPECT_FALSE(create(badParameters).has_value());
    Json badMode = minimal();
    badMode["executionMode"] = "sometimes";
    EXPECT_FALSE(create(badMode).has_value());
    EXPECT_FALSE(m_factory.create(minimal().dump(), nullptr, nullptr).has_value());
}

TEST_F(PluginToolFactoryTest, ExecutePassesArgumentsAndConvertsTheResult) {
    const auto result = run(R"({"content":[{"type":"text","text":"hi"},{"type":"image","data":"AA==","mimeType":"image/png"}],)"
                            R"("details":{"k":1},"structuredContent":{"s":2},"isError":true,"terminate":true})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_recorder.toolCallId, "call-7");
    EXPECT_EQ(Json::parse(m_recorder.params)["who"], "me");
    EXPECT_FALSE(m_recorder.aborted);
    ASSERT_EQ(result->content.size(), 2U);
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "hi");
    EXPECT_EQ(std::get<ImageContent>(result->content[1]).mimeType, "image/png");
    EXPECT_EQ(result->details["k"], 1);
    EXPECT_EQ(result->structuredContent["s"], 2);
    EXPECT_TRUE(result->isError);
    EXPECT_TRUE(result->terminate);
}

TEST_F(PluginToolFactoryTest, AbortHandleIsTheCallsSignal) {
    m_signal->abort();
    ASSERT_TRUE(run(R"({"content":[]})").has_value());
    EXPECT_TRUE(m_recorder.aborted);
}

TEST_F(PluginToolFactoryTest, PartialResultsStream) {
    m_recorder.sendUpdate = true;
    std::vector<std::string> updates;
    const auto result = run(R"({"content":[]})", [&updates](const AgentToolResult& update) {
        updates.push_back(std::get<TextContent>(update.content.at(0)).text);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(updates, std::vector<std::string>{"working"});
}

TEST_F(PluginToolFactoryTest, FailuresBecomeErrors) {
    const auto failed = run(R"({"error":"nope"})");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "nope");
    EXPECT_FALSE(run("garbage").has_value());
    EXPECT_FALSE(run(R"({"content":"text"})").has_value());
    EXPECT_FALSE(run(R"({"content":[{"type":"video"}]})").has_value());
    m_recorder.replyNothing = true;
    const auto nothing = run("");
    ASSERT_FALSE(nothing.has_value());
    EXPECT_EQ(nothing.error().message, "plugin tool returned nothing");
}
