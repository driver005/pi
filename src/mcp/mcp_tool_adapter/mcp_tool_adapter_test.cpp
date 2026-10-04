#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.mcp.mcp_tool_adapter;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class RecordingCaller : public IMcpToolCaller {
public:
    Result<McpCallResult> callTool(const std::string& name, const Json& arguments,
                                   const McpRequestOptions& options) override {
        m_calls.emplace_back(name, arguments);
        m_lastOptions = options;
        if (options.onProgress) {
            McpProgress progress;
            progress.progress = 2;
            progress.total = 5;
            options.onProgress(progress);
            McpProgress described;
            described.message = "halfway";
            options.onProgress(described);
        }
        return m_reply;
    }

    Result<McpCallResult> m_reply = McpCallResult{};
    std::vector<std::pair<std::string, Json>> m_calls;
    McpRequestOptions m_lastOptions;
};

class McpToolAdapterTest : public testing::Test {
protected:
    McpTool tool(const std::string& name = "search") {
        McpTool out;
        out.name = name;
        out.inputSchema = Json{{"properties", Json{{"q", Json{{"type", "string"}}}}}};
        return out;
    }

    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    McpResultConverter m_converter{m_files, m_crypto, m_base64, m_environment};
    RecordingCaller m_caller;
};

TEST_F(McpToolAdapterTest, DefinitionDescribesTheTool) {
    McpTool described = tool();
    described.description = "  Find things.\n";
    McpToolAdapter adapter("docs", described, "mcp__docs__search", m_caller, m_converter, 5000);
    EXPECT_EQ(adapter.definition().name, "mcp__docs__search");
    EXPECT_EQ(adapter.definition().description, "Find things.");
    EXPECT_EQ(adapter.definition().parameters["type"], "object");
    EXPECT_EQ(adapter.definition().parameters["properties"]["q"]["type"], "string");
    EXPECT_EQ(adapter.label(), "docs/search");
    EXPECT_EQ(adapter.promptSnippet(), "");
    EXPECT_FALSE(adapter.executionMode().has_value());
}

TEST_F(McpToolAdapterTest, DescriptionFallsBackToTitleThenAName) {
    McpTool titled = tool();
    titled.title = "Search Docs";
    EXPECT_EQ(McpToolAdapter("docs", titled, "n", m_caller, m_converter, 1).definition().description, "Search Docs");
    McpTool annotated = tool();
    annotated.annotations = Json{{"title", "From annotations"}};
    EXPECT_EQ(McpToolAdapter("docs", annotated, "n", m_caller, m_converter, 1).definition().description,
              "From annotations");
    EXPECT_EQ(McpToolAdapter("docs", tool(), "n", m_caller, m_converter, 1).definition().description,
              "MCP tool search from server docs");
}

TEST_F(McpToolAdapterTest, SchemaWithoutShapeBecomesAnObjectSchema) {
    McpTool bare = tool();
    bare.inputSchema = Json::object();
    const McpToolAdapter adapter("docs", bare, "n", m_caller, m_converter, 1);
    EXPECT_EQ(adapter.definition().parameters, (Json{{"type", "object"}, {"properties", Json::object()}}));
}

TEST_F(McpToolAdapterTest, ExecuteCallsTheServerAndConvertsTheResult) {
    McpToolAdapter adapter("docs", tool(), "mcp__docs__search", m_caller, m_converter, 7000);
    McpCallResult reply;
    reply.content = Json::array({Json{{"type", "text"}, {"text", "found"}}});
    m_caller.m_reply = reply;
    std::vector<std::string> updates;
    const auto signal = std::make_shared<AbortSignal>();
    const auto result = adapter.execute("call-1", Json{{"q", "x"}}, signal, [&updates](const AgentToolResult& update) {
        updates.push_back(std::get<TextContent>(update.content.at(0)).text);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content.at(0)).text, "found");
    EXPECT_EQ(result->details["server"], "docs");
    ASSERT_EQ(m_caller.m_calls.size(), 1U);
    EXPECT_EQ(m_caller.m_calls[0].first, "search");
    EXPECT_EQ(m_caller.m_calls[0].second["q"], "x");
    EXPECT_EQ(m_caller.m_lastOptions.signal, signal);
    EXPECT_EQ(*m_caller.m_lastOptions.timeoutMs, 7000);
    EXPECT_EQ(updates, (std::vector<std::string>{"Progress 2/5", "halfway"}));
}

TEST_F(McpToolAdapterTest, ServerErrorsAndErrorResultsReachTheModel) {
    McpToolAdapter adapter("docs", tool(), "n", m_caller, m_converter, 1);
    m_caller.m_reply = std::unexpected(Error{"timeout", "MCP request timed out"});
    const auto failed = adapter.execute("c", Json::object(), nullptr, nullptr);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "timeout");

    McpCallResult isError;
    isError.isError = true;
    isError.content = Json::array({Json{{"type", "text"}, {"text", "bad input"}}});
    m_caller.m_reply = isError;
    const auto result = adapter.execute("c", Json::array(), nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isError);
    EXPECT_EQ(m_caller.m_calls.back().second, Json::object());
}
