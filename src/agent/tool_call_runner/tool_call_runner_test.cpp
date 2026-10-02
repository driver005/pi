#include "src/agent/tool_call_runner/tool_call_runner.h"

#include <gtest/gtest.h>

class EchoTool : public ITool {
public:
    EchoTool() {
        m_definition.name = "echo";
        m_definition.description = "Echo";
        m_definition.parameters = Json::parse(
            R"({"type":"object","properties":{"text":{"type":"string"},"n":{"type":"integer"}},"required":["text"]})");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "Echo";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }
    Result<AgentToolResult> execute(const std::string&, const Json& params,
                                    const std::shared_ptr<AbortSignal>&,
                                    const ToolUpdateCallback& onUpdate) override {
        ++m_executions;
        if (params.value("text", "") == "fail") {
            return std::unexpected(Error{"boom", "tool exploded"});
        }
        AgentToolResult partial;
        partial.content.emplace_back(TextContent{"partial", std::nullopt});
        onUpdate(partial);
        AgentToolResult result;
        result.content.emplace_back(TextContent{params["text"].get<std::string>(), std::nullopt});
        result.details = params;
        return result;
    }

    int m_executions = 0;

private:
    Tool m_definition;
};

class ToolCallRunnerTest : public testing::Test {
protected:
    ToolCall call(const std::string& name, const Json& args) {
        ToolCall c;
        c.id = "c1";
        c.name = name;
        c.arguments = args;
        return c;
    }

    std::string textOf(const AgentToolResult& result) {
        return std::get<TextContent>(result.content[0]).text;
    }

    ToolCallOutcome runCall(const ToolCall& c) {
        return m_runner.run(m_context, m_assistant, c, m_hooks, nullptr, nullptr);
    }

    ToolCallRunner m_runner;
    std::shared_ptr<EchoTool> m_tool = std::make_shared<EchoTool>();
    AgentContext m_context = [this] {
        AgentContext context;
        context.tools.push_back(m_tool);
        return context;
    }();
    AssistantMessage m_assistant;
    AgentLoopConfig m_hooks;
};

TEST_F(ToolCallRunnerTest, ExecutesWithValidatedAndCoercedArguments) {
    const auto outcome = runCall(call("echo", Json::parse(R"({"text":"hi","n":"3"})")));
    EXPECT_FALSE(outcome.isError);
    EXPECT_EQ(textOf(outcome.result), "hi");
    EXPECT_EQ(outcome.result.details["n"], 3);
}

TEST_F(ToolCallRunnerTest, UnknownToolIsErrorResult) {
    const auto outcome = runCall(call("nope", Json::object()));
    EXPECT_TRUE(outcome.isError);
    EXPECT_EQ(textOf(outcome.result), "Tool nope not found");
}

TEST_F(ToolCallRunnerTest, ValidationFailureListsIssuesAndArguments) {
    const auto outcome = runCall(call("echo", Json::object()));
    EXPECT_TRUE(outcome.isError);
    const std::string text = textOf(outcome.result);
    EXPECT_NE(text.find("Validation failed for tool \"echo\":"), std::string::npos);
    EXPECT_NE(text.find("  - text: Expected required property"), std::string::npos);
    EXPECT_NE(text.find("Received arguments:"), std::string::npos);
    EXPECT_EQ(m_tool->m_executions, 0);
}

TEST_F(ToolCallRunnerTest, ToolErrorBecomesErrorResult) {
    const auto outcome = runCall(call("echo", Json::parse(R"({"text":"fail"})")));
    EXPECT_TRUE(outcome.isError);
    EXPECT_EQ(textOf(outcome.result), "tool exploded");
}

TEST_F(ToolCallRunnerTest, BeforeHookCanBlock) {
    m_hooks.beforeToolCall = [](const ToolCallContext&, const std::shared_ptr<AbortSignal>&) {
        BeforeToolCallResult result;
        result.block = true;
        result.reason = "not allowed";
        result.terminate = true;
        return std::optional<BeforeToolCallResult>(result);
    };
    const auto outcome = runCall(call("echo", Json::parse(R"({"text":"hi"})")));
    EXPECT_TRUE(outcome.isError);
    EXPECT_TRUE(outcome.result.terminate);
    EXPECT_EQ(textOf(outcome.result), "not allowed");
    EXPECT_EQ(m_tool->m_executions, 0);
}

TEST_F(ToolCallRunnerTest, AfterHookOverridesResult) {
    m_hooks.afterToolCall = [](const ToolCallContext& context, const std::shared_ptr<AbortSignal>&) {
        EXPECT_TRUE(context.result.has_value());
        AfterToolCallResult override;
        override.content = std::vector<UserContentBlock>{TextContent{"changed", std::nullopt}};
        override.isError = true;
        return std::optional<AfterToolCallResult>(override);
    };
    const auto outcome = runCall(call("echo", Json::parse(R"({"text":"hi"})")));
    EXPECT_TRUE(outcome.isError);
    EXPECT_EQ(textOf(outcome.result), "changed");
    EXPECT_EQ(outcome.result.details["text"], "hi");
}

TEST_F(ToolCallRunnerTest, UpdatesReachCallbackDuringExecution) {
    std::vector<std::string> updates;
    const auto outcome = m_runner.run(m_context, m_assistant, call("echo", Json::parse(R"({"text":"x"})")),
                                      m_hooks, nullptr, [&](const AgentToolResult& partial) {
                                          updates.push_back(textOf(partial));
                                      });
    EXPECT_FALSE(outcome.isError);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0], "partial");
}

TEST_F(ToolCallRunnerTest, AbortedBeforeExecutionSettlesImmediately) {
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    const auto outcome = m_runner.run(m_context, m_assistant, call("echo", Json::parse(R"({"text":"hi"})")),
                                      m_hooks, signal, nullptr);
    EXPECT_TRUE(outcome.isError);
    EXPECT_EQ(textOf(outcome.result), "Operation aborted");
    EXPECT_EQ(m_tool->m_executions, 0);
}
