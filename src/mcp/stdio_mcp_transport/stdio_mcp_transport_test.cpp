#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.child_process_launcher;
import pi.mcp.mcp_client;
import pi.mcp.stdio_mcp_transport;

class StdioMcpTransportTest : public testing::Test {
protected:
    McpStdioOptions shell(const std::string& script) {
        McpStdioOptions options;
        options.command = "/bin/sh";
        options.args = {"-c", script};
        options.closeTimeoutMs = 500;
        return options;
    }

    /** A tiny MCP server in sh: answers initialize, tools/list and tools/call by matching the method name. */
    std::string serverScript() {
        return R"SH(
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sh","version":"1"}}}\n' "$id";;
    *'"method":"tools/list"'*)
      printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[{"name":"echo","description":"Echo","inputSchema":{"type":"object"}}]}}\n' "$id";;
    *'"method":"tools/call"'*)
      printf '{"jsonrpc":"2.0","id":%s,"result":{"content":[{"type":"text","text":"called"}]}}\n' "$id";;
  esac
done
)SH";
    }

    void waitUntil(const std::function<bool()>& condition) {
        for (int i = 0; i < 600 && !condition(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ChildProcessLauncher m_launcher;
};

TEST_F(StdioMcpTransportTest, DrivesAShellServerThroughTheClient) {
    McpClient client{McpClientOptions{}};
    auto transport = std::make_unique<StdioMcpTransport>(m_launcher, shell(serverScript()));
    ASSERT_TRUE(client.connect(std::move(transport)).has_value());
    const auto tools = client.listTools({});
    ASSERT_TRUE(tools.has_value());
    ASSERT_EQ(tools->size(), 1U);
    EXPECT_EQ((*tools)[0].name, "echo");
    const auto result = client.callTool("echo", Json::object(), {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content[0]["text"], "called");
    client.close();
    EXPECT_FALSE(client.connected());
}

TEST_F(StdioMcpTransportTest, MessagesSplitAcrossReadsAreReassembled) {
    StdioMcpTransport transport(m_launcher, shell(R"(printf '{"jsonrpc":"2.0",'; sleep 0.1; printf '"method":"notifications/x"}\n'; sleep 0.2)"));
    std::mutex mutex;
    std::vector<Json> messages;
    transport.setMessageListener([&](const Json& message) {
        const std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(message);
    });
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return !messages.empty();
    });
    transport.close();
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0]["method"], "notifications/x");
}

TEST_F(StdioMcpTransportTest, InvalidLinesAreReportedAndSkipped) {
    StdioMcpTransport transport(m_launcher, shell(R"(echo 'not json'; echo '{"jsonrpc":"2.0","method":"ok"}'; echo '[]'; sleep 0.2)"));
    std::mutex mutex;
    std::vector<std::string> methods;
    int errors = 0;
    transport.setMessageListener([&](const Json& message) {
        const std::lock_guard<std::mutex> lock(mutex);
        methods.push_back(message["method"]);
    });
    transport.setErrorListener([&](const Error&) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++errors;
    });
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return errors >= 2 && !methods.empty();
    });
    transport.close();
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(methods, (std::vector<std::string>{"ok"}));
    EXPECT_EQ(errors, 2);
}

TEST_F(StdioMcpTransportTest, StderrIsKeptAsABoundedTail) {
    McpStdioOptions options = shell(R"(printf 'AAAAAAAAAA' >&2; printf 'BBBBB' >&2; sleep 0.2)");
    options.maxStderrBytes = 8;
    StdioMcpTransport transport(m_launcher, options);
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] { return transport.stderrTail().size() == 8 && transport.stderrTail().back() == 'B'; });
    EXPECT_EQ(transport.stderrTail(), "AAABBBBB");
    transport.close();
}

TEST_F(StdioMcpTransportTest, ServerExitEmitsCloseOnce) {
    StdioMcpTransport transport(m_launcher, shell("exit 0"));
    std::atomic<int> closes{0};
    transport.setCloseListener([&] { ++closes; });
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] { return closes.load() > 0; });
    transport.close();
    EXPECT_EQ(closes.load(), 1);
    EXPECT_FALSE(transport.send(Json::object()).has_value());
}

TEST_F(StdioMcpTransportTest, IncompleteTrailingMessageIsAnError) {
    StdioMcpTransport transport(m_launcher, shell(R"(printf '{"jsonrpc":"2.0"')"));
    std::atomic<int> errors{0};
    transport.setErrorListener([&](const Error&) { ++errors; });
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] { return errors.load() > 0; });
    transport.close();
    EXPECT_EQ(errors.load(), 1);
}

TEST_F(StdioMcpTransportTest, CloseEscalatesToTerminateForServersThatIgnoreStdin) {
    StdioMcpTransport transport(m_launcher, shell("while true; do sleep 1; done"));
    ASSERT_TRUE(transport.start().has_value());
    const auto started = std::chrono::steady_clock::now();
    transport.close();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
    transport.close();
}

TEST_F(StdioMcpTransportTest, MissingCommandsFailToStart) {
    McpStdioOptions options;
    options.command = "/definitely/not/here";
    StdioMcpTransport transport(m_launcher, options);
    const auto started = transport.start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, "spawn_failed");
}

TEST_F(StdioMcpTransportTest, EnvironmentAndWorkingDirectoryApply) {
    McpStdioOptions options = shell(R"sh(printf '{"jsonrpc":"2.0","method":"env","params":{"v":"%s","d":"%s"}}\n' "$PI_MCP_TEST" "$(pwd)"; sleep 0.2)sh");
    options.env = {{"PI_MCP_TEST", "yes"}};
    options.cwd = "/tmp";
    StdioMcpTransport transport(m_launcher, options);
    std::mutex mutex;
    std::optional<Json> seen;
    transport.setMessageListener([&](const Json& message) {
        const std::lock_guard<std::mutex> lock(mutex);
        seen = message;
    });
    ASSERT_TRUE(transport.start().has_value());
    waitUntil([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return seen.has_value();
    });
    transport.close();
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ((*seen)["params"]["v"], "yes");
    EXPECT_EQ((*seen)["params"]["d"], "/tmp");
}
