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
    EXPECT_NE(m_parser.usage().find("Usage: pi rpc|serve"), std::string::npos);
}

TEST_F(CommandLineParserTest, ReportsUsageErrors) {
    EXPECT_FALSE(parse({"mcp", "list", "--bogus"}).has_value());
    EXPECT_FALSE(parse({"-x"}).has_value());
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

TEST_F(CommandLineParserTest, UnknownFlagsOfRpcAndServeAreLeftToThePlugins) {
    const auto line = parse({"rpc", "--verbose", "--plan", "fast", "--level=3", "--no-tools", "--last"});
    ASSERT_TRUE(line.has_value());
    const auto& flags = line->options.startup.pluginFlags;
    ASSERT_EQ(flags.size(), 4U);
    EXPECT_FALSE(flags.at("verbose").has_value());
    EXPECT_EQ(flags.at("plan"), "fast");
    EXPECT_EQ(flags.at("level"), "3");
    EXPECT_FALSE(flags.at("last").has_value());
    EXPECT_TRUE(line->options.startup.noTools);
    const auto served = parse({"serve", "--audit"});
    ASSERT_TRUE(served.has_value());
    EXPECT_EQ(served->options.startup.pluginFlags.count("audit"), 1U);
}

TEST_F(CommandLineParserTest, ServeDefaultsToTheHomeServerDirectoryAndItsOwnSessions) {
    const auto line = parse({"serve"});
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->command, "serve");
    EXPECT_EQ(line->serverDir, "/home/me/.pi/server");
    EXPECT_FALSE(line->serverId.has_value());
    EXPECT_EQ(line->options.sessionDir, "/home/me/.pi/agent/server-sessions");
}

TEST_F(CommandLineParserTest, ServeOptionsComeFromEnvironmentThenFlags) {
    m_environment.set("PI_SERVER_DIR", "~/srv");
    m_environment.set("PI_SERVER_ID", "00000000-0000-4000-8000-000000000001");
    const auto fromEnvironment = parse({"serve"});
    EXPECT_EQ(fromEnvironment->serverDir, "/home/me/srv");
    EXPECT_EQ(fromEnvironment->serverId, "00000000-0000-4000-8000-000000000001");
    const auto fromFlags = parse({"serve", "--server-dir", "/run/pi", "--server-id", "abc", "--session-dir", "/data"});
    EXPECT_EQ(fromFlags->serverDir, "/run/pi");
    EXPECT_EQ(fromFlags->serverId, "abc");
    EXPECT_EQ(fromFlags->options.sessionDir, "/data");
    EXPECT_FALSE(parse({"serve", "--server-dir"}).has_value());
}

TEST_F(CommandLineParserTest, ServeKeepsDurableSessionsUnlessSessionTreesAreAsked) {
    EXPECT_FALSE(parse({"serve"})->sessionTree);
    EXPECT_TRUE(parse({"serve", "--session-tree"})->sessionTree);
    EXPECT_NE(m_parser.usage().find("--session-tree"), std::string::npos);
}

TEST_F(CommandLineParserTest, RpcLeavesTheServeOptionsUnset) {
    const auto line = parse({"rpc"});
    EXPECT_TRUE(line->serverDir.empty());
    EXPECT_FALSE(line->options.sessionDir.has_value());
}

TEST_F(CommandLineParserTest, ParsesTheMcpCommands) {
    const auto login = parse({"mcp", "login", "docs", "--agent-dir", "/a", "--trust"});
    ASSERT_TRUE(login.has_value()) << login.error().message;
    EXPECT_EQ(login->command, "mcp");
    EXPECT_EQ(login->arguments, (std::vector<std::string>{"login", "docs"}));
    EXPECT_EQ(login->options.agentDir, "/a");
    EXPECT_EQ(parse({"mcp", "logout", "docs"})->arguments, (std::vector<std::string>{"logout", "docs"}));
    EXPECT_EQ(parse({"mcp", "list"})->arguments, std::vector<std::string>{"list"});
    EXPECT_EQ(parse({"mcp", "--cwd", "/p", "list"})->options.cwd, "/p");
}

TEST_F(CommandLineParserTest, RejectsMalformedMcpCommands) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{{"mcp"}, {"mcp", "login"}, {"mcp", "login", "a", "b"}, {"mcp", "list", "x"}, {"mcp", "frobnicate", "a"}}) {
        const auto line = parse(args);
        ASSERT_FALSE(line.has_value());
        EXPECT_NE(line.error().message.find("Usage: pi mcp"), std::string::npos);
    }
}

TEST_F(CommandLineParserTest, ParsesTheAuthCommands) {
    const auto login = parse({"auth", "login", "anthropic", "--method", "copy_code", "--manual", "--agent-dir", "/a"});
    ASSERT_TRUE(login.has_value()) << login.error().message;
    EXPECT_EQ(login->command, "auth");
    EXPECT_EQ(login->arguments, (std::vector<std::string>{"login", "anthropic"}));
    EXPECT_EQ(login->loginMethod, "copy_code");
    EXPECT_TRUE(login->loginManual);
    EXPECT_EQ(login->options.agentDir, "/a");
    EXPECT_EQ(parse({"auth", "logout", "xai"})->arguments, (std::vector<std::string>{"logout", "xai"}));
    EXPECT_EQ(parse({"auth", "status"})->arguments, std::vector<std::string>{"status"});
    EXPECT_EQ(parse({"auth", "list"})->arguments, std::vector<std::string>{"list"});
    EXPECT_FALSE(parse({"auth", "login", "x"})->loginManual);
    EXPECT_TRUE(parse({"auth", "login", "x"})->loginMethod.empty());
}

TEST_F(CommandLineParserTest, RejectsMalformedAuthCommands) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{{"auth"}, {"auth", "login"}, {"auth", "login", "a", "b"}, {"auth", "status", "x"}, {"auth", "frobnicate"}}) {
        const auto line = parse(args);
        ASSERT_FALSE(line.has_value());
        EXPECT_NE(line.error().message.find("Usage: pi auth"), std::string::npos);
    }
}

TEST_F(CommandLineParserTest, ParsesThePackageCommands) {
    const auto install = parse({"install", "git:github.com/a/b@v1", "-l"});
    ASSERT_TRUE(install.has_value()) << install.error().message;
    EXPECT_EQ(install->command, "install");
    EXPECT_EQ(install->arguments, std::vector<std::string>{"git:github.com/a/b@v1"});
    EXPECT_TRUE(install->localPackages);
    EXPECT_FALSE(parse({"remove", "./x"})->localPackages);
    EXPECT_TRUE(parse({"update"})->arguments.empty());
    EXPECT_EQ(parse({"update", "x"})->arguments, std::vector<std::string>{"x"});
    EXPECT_EQ(parse({"list", "--local"})->command, "list");
}

TEST_F(CommandLineParserTest, RejectsMalformedPackageCommands) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{{"install"}, {"install", "a", "b"}, {"remove"}, {"update", "a", "b"}, {"list", "x"}}) {
        const auto line = parse(args);
        ASSERT_FALSE(line.has_value());
        EXPECT_NE(line.error().message.find("Usage: pi install"), std::string::npos);
    }
}

TEST_F(CommandLineParserTest, OtherCommandsTakeNoPositionalArguments) {
    const auto line = parse({"serve", "extra"});
    ASSERT_FALSE(line.has_value());
    EXPECT_EQ(line.error().code, "usage");
}

TEST_F(CommandLineParserTest, ParsesTheExportCommand) {
    const auto line = parse({"export", "session.jsonl", "out.html", "--theme", "light"});
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->command, "export");
    EXPECT_EQ(line->arguments, (std::vector<std::string>{"session.jsonl", "out.html"}));
    EXPECT_EQ(line->exportTheme, "light");
    EXPECT_TRUE(parse({"export", "session.jsonl"})->exportTheme.empty());
    EXPECT_FALSE(parse({"export"}).has_value());
    EXPECT_FALSE(parse({"export", "a", "b", "c"}).has_value());
}

TEST_F(CommandLineParserTest, ParsesTheEvalsCommands) {
    const auto plan = parse({"evals", "plan", "found.json", "--model", "p/m", "--runs", "3"});
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->arguments, (std::vector<std::string>{"plan", "found.json"}));
    EXPECT_EQ(plan->options.startup.model, "p/m");
    EXPECT_EQ(plan->evalRuns, 3);
    EXPECT_EQ(parse({"evals", "plan", "found.json"})->evalRuns, 1);
    EXPECT_TRUE(parse({"evals", "report", "dir"}).has_value());
    EXPECT_TRUE(parse({"evals", "observe", "task.json", "report.json"}).has_value());
    EXPECT_FALSE(parse({"evals"}).has_value());
    EXPECT_FALSE(parse({"evals", "report"}).has_value());
    EXPECT_FALSE(parse({"evals", "observe", "task.json"}).has_value());
    EXPECT_FALSE(parse({"evals", "run", "x"}).has_value());
    EXPECT_FALSE(parse({"evals", "plan", "f", "--runs", "0"}).has_value());
    EXPECT_FALSE(parse({"evals", "plan", "f", "--runs", "many"}).has_value());
}
