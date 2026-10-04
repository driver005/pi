#include <gtest/gtest.h>

import std;
import pi.support.config_value_resolver;
import pi.testing.fake_environment;
import pi.testing.scripted_process_runner;

class ConfigValueResolverTest : public testing::Test {
protected:
    ConfigValueResolverTest()
        : m_environment({{"KEY", "secret"}, {"A", "1"}, {"B", "2"}}),
          m_processes([this](const ProcessRequest& request) -> Result<ProcessResult> {
              ProcessResult result;
              result.output = m_output;
              result.exitCode = m_exit;
              m_lastScript = request.args.empty() ? "" : request.args.back();
              return result;
          }),
          m_resolver(m_environment, m_processes) {}

    FakeEnvironment m_environment;
    std::string m_output = "  from-command \n";
    int m_exit = 0;
    std::string m_lastScript;
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_resolver;
};

TEST_F(ConfigValueResolverTest, LiteralPassesThrough) {
    EXPECT_EQ(m_resolver.resolve("plain-key", {}), "plain-key");
    EXPECT_EQ(m_resolver.resolve("", {}), "");
}

TEST_F(ConfigValueResolverTest, EnvReferencesInterpolate) {
    EXPECT_EQ(m_resolver.resolve("$KEY", {}), "secret");
    EXPECT_EQ(m_resolver.resolve("Bearer ${KEY}!", {}), "Bearer secret!");
    EXPECT_EQ(m_resolver.resolve("$A-$B", {}), "1-2");
}

TEST_F(ConfigValueResolverTest, MissingEnvVarIsUnresolved) {
    EXPECT_FALSE(m_resolver.resolve("$NOPE", {}).has_value());
    EXPECT_FALSE(m_resolver.resolve("x$NOPE", {}).has_value());
}

TEST_F(ConfigValueResolverTest, ExplicitEnvWinsOverProcessEnv) {
    EXPECT_EQ(m_resolver.resolve("$KEY", {{"KEY", "override"}}), "override");
}

TEST_F(ConfigValueResolverTest, EscapesAndLoneDollars) {
    EXPECT_EQ(m_resolver.resolve("$$KEY", {}), "$KEY");
    EXPECT_EQ(m_resolver.resolve("$!bang", {}), "!bang");
    EXPECT_EQ(m_resolver.resolve("cost: 5$", {}), "cost: 5$");
    EXPECT_EQ(m_resolver.resolve("${not valid}", {}), "${not valid}");
    EXPECT_EQ(m_resolver.resolve("${open", {}), "${open");
}

TEST_F(ConfigValueResolverTest, CommandsRunThroughShellAndAreCached) {
    EXPECT_EQ(m_resolver.resolve("!pass show x", {}), "from-command");
    EXPECT_EQ(m_resolver.resolve("!pass show x", {}), "from-command");
    EXPECT_EQ(m_processes.calls(), 1);
    EXPECT_NE(m_lastScript.find("pass show x"), std::string::npos);
    m_resolver.clearCache();
    EXPECT_EQ(m_resolver.resolve("!pass show x", {}), "from-command");
    EXPECT_EQ(m_processes.calls(), 2);
}

TEST_F(ConfigValueResolverTest, FailingOrEmptyCommandIsUnresolved) {
    m_exit = 1;
    EXPECT_FALSE(m_resolver.resolve("!fail", {}).has_value());
    m_exit = 0;
    m_output = "  \n";
    EXPECT_FALSE(m_resolver.resolve("!blank", {}).has_value());
}

TEST_F(ConfigValueResolverTest, UncachedAlwaysRuns) {
    m_resolver.resolveUncached("!x", {});
    m_resolver.resolveUncached("!x", {});
    EXPECT_EQ(m_processes.calls(), 2);
}

TEST_F(ConfigValueResolverTest, NameIntrospection) {
    EXPECT_EQ(m_resolver.envVarName("$KEY"), "KEY");
    EXPECT_EQ(m_resolver.envVarName("${KEY}"), "KEY");
    EXPECT_FALSE(m_resolver.envVarName("a$KEY").has_value());
    EXPECT_FALSE(m_resolver.envVarName("!cmd").has_value());
    EXPECT_EQ(m_resolver.envVarNames("$A $B $A"), (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(m_resolver.missingEnvVarNames("$A $X $Y", {}), (std::vector<std::string>{"X", "Y"}));
    EXPECT_TRUE(m_resolver.isConfigured("$A", {}));
    EXPECT_FALSE(m_resolver.isConfigured("$X", {}));
    EXPECT_TRUE(m_resolver.isCommand("!x"));
    EXPECT_FALSE(m_resolver.isCommand("x"));
}

TEST_F(ConfigValueResolverTest, ErrorMessagesNameTheSource) {
    EXPECT_EQ(m_resolver.resolveOrError("$X", "API key", {}).error().message,
              "Failed to resolve API key from environment variable: X");
    EXPECT_EQ(m_resolver.resolveOrError("$X$Y", "API key", {}).error().message,
              "Failed to resolve API key from environment variables: X, Y");
    m_exit = 2;
    EXPECT_EQ(m_resolver.resolveOrError("!boom now", "API key", {}).error().message,
              "Failed to resolve API key from shell command: boom now");
}

TEST_F(ConfigValueResolverTest, HeadersDropUnresolvedEntries) {
    const auto headers = m_resolver.resolveHeaders({{"x-a", "$KEY"}, {"x-b", "$NOPE"}, {"x-c", "lit"}}, {});
    EXPECT_EQ(headers.size(), 2U);
    EXPECT_EQ(headers.at("x-a"), "secret");
    const auto strict = m_resolver.resolveHeadersOrError({{"x-b", "$NOPE"}}, "model", {});
    ASSERT_FALSE(strict.has_value());
    EXPECT_EQ(strict.error().message,
              "Failed to resolve model header \"x-b\" from environment variable: NOPE");
}
