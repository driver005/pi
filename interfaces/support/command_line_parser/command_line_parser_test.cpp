#include <gtest/gtest.h>

import std;
import pi.support.command_line_parser;
import pi.testing.fake_environment;

class CommandLineParserTest : public testing::Test {
protected:
    Result<CommandLine> parse(const std::vector<std::string>& args) {
        return m_parser.parse(args, "/work");
    }

    FakeEnvironment m_environment{{{"HOME", "/home/me"}}};
    CommandLineParser m_parser{m_environment};
};

TEST_F(CommandLineParserTest, DefaultsToRpcInCurrentDirectory) {
    const auto line = parse({});
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->command, "rpc");
    EXPECT_EQ(line->options.cwd, "/work");
    EXPECT_EQ(line->options.agentDir, "/home/me/.pi/agent");
    EXPECT_EQ(line->options.sessionMode, SessionStartMode::New);
}

TEST_F(CommandLineParserTest, AgentDirComesFromEnvironmentThenFlag) {
    m_environment.set("PI_CODING_AGENT_DIR", "~/agent");
    EXPECT_EQ(parse({"rpc"})->options.agentDir, "/home/me/agent");
    EXPECT_EQ(parse({"rpc", "--agent-dir", "/x"})->options.agentDir, "/x");
}

TEST_F(CommandLineParserTest, SessionFlagsSetTheStartMode) {
    EXPECT_EQ(parse({"--continue"})->options.sessionMode, SessionStartMode::Continue);
    EXPECT_EQ(parse({"-c"})->options.sessionMode, SessionStartMode::Continue);
    EXPECT_EQ(parse({"--no-session"})->options.sessionMode, SessionStartMode::InMemory);
    const auto open = parse({"--session", "abc"});
    EXPECT_EQ(open->options.sessionMode, SessionStartMode::Open);
    EXPECT_EQ(open->options.sessionRef, "abc");
}

TEST_F(CommandLineParserTest, StartupOptions) {
    const auto line = parse({"rpc", "--model", "a/b:high", "--thinking", "low", "--tools", "read,ls", "--no-skills",
                             "--append-system-prompt", "one", "--append-system-prompt", "two", "--skill", "~/s",
                             "--trust", "--faux"});
    ASSERT_TRUE(line.has_value());
    const CodingStartupOptions& startup = line->options.startup;
    EXPECT_EQ(startup.model, "a/b:high");
    EXPECT_EQ(startup.thinking, "low");
    EXPECT_EQ(startup.tools, (std::vector<std::string>{"read", "ls"}));
    EXPECT_TRUE(startup.noSkills);
    EXPECT_EQ(startup.appendSystemPrompt, (std::vector<std::string>{"one", "two"}));
    EXPECT_EQ(startup.skillPaths, (std::vector<std::string>{"/home/me/s"}));
    EXPECT_EQ(startup.trustProject, true);
    EXPECT_TRUE(line->options.faux);
}

TEST_F(CommandLineParserTest, HelpClearsTheCommand) {
    const auto line = parse({"--help"});
    ASSERT_TRUE(line.has_value());
    EXPECT_TRUE(line->help);
    EXPECT_TRUE(line->command.empty());
    EXPECT_NE(m_parser.usage().find("Usage: pi rpc"), std::string::npos);
}

TEST_F(CommandLineParserTest, ReportsUsageErrors) {
    EXPECT_FALSE(parse({"--bogus"}).has_value());
    EXPECT_FALSE(parse({"--model"}).has_value());
    EXPECT_FALSE(parse({"serve2"}).has_value());
}

TEST_F(CommandLineParserTest, McpOptions) {
    const auto line = parse({"rpc", "--no-mcp", "--mcp-wait", "1500"});
    ASSERT_TRUE(line.has_value());
    EXPECT_TRUE(line->options.startup.noMcp);
    EXPECT_EQ(line->options.startup.mcpStartupWaitMs, 1500);
    EXPECT_FALSE(parse({"rpc", "--mcp-wait", "soon"}).has_value());
    EXPECT_FALSE(parse({"rpc", "--mcp-wait", "-5"}).has_value());
    EXPECT_FALSE(parse({"rpc"})->options.startup.noMcp);
    EXPECT_EQ(parse({"rpc"})->options.startup.mcpStartupWaitMs, 5000);
}

TEST_F(CommandLineParserTest, PluginOptions) {
    const auto line = parse({"rpc", "--plugin", "~/a.so", "--plugin", "/b.so", "--no-plugins"});
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->options.startup.pluginPaths, (std::vector<std::string>{"/home/me/a.so", "/b.so"}));
    EXPECT_TRUE(line->options.startup.noPlugins);
    EXPECT_FALSE(parse({"rpc"})->options.startup.noPlugins);
}
