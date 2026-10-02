#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mcp_server_config_validator;

class McpServerConfigValidatorTest : public testing::Test {
protected:
    std::string failure(const std::string& name, const Json& raw) {
        const auto result = m_validator.validate(name, raw);
        EXPECT_FALSE(result.has_value());
        return result ? "" : result.error().message;
    }

    McpServerConfigValidator m_validator;
};

TEST_F(McpServerConfigValidatorTest, AcceptsStdioServers) {
    const auto config = m_validator.validate(
        "fs", Json{{"command", "npx"}, {"args", Json::array({"-y", "server"})}, {"env", Json{{"K", "${V}"}}},
                   {"cwd", "sub"}, {"timeout", 5}, {"enabled", false}, {"description", "files"}});
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->http);
    EXPECT_EQ(config->command, "npx");
    EXPECT_EQ(config->args, (std::vector<std::string>{"-y", "server"}));
    EXPECT_EQ(config->env.at("K"), "${V}");
    EXPECT_EQ(*config->cwd, "sub");
    EXPECT_EQ(*config->timeoutSeconds, 5);
    EXPECT_FALSE(config->enabled);
    EXPECT_EQ(*config->description, "files");
    EXPECT_EQ(config->exposure, McpExposure::Codemode);
}

TEST_F(McpServerConfigValidatorTest, AcceptsHttpServers) {
    const auto config = m_validator.validate(
        "docs", Json{{"url", "https://example.com/mcp"}, {"headers", Json{{"Authorization", "Bearer ${T}"}}},
                     {"type", "streamable-http"}, {"auth", Json{{"provider", "anthropic"}}}});
    ASSERT_TRUE(config.has_value());
    EXPECT_TRUE(config->http);
    EXPECT_EQ(config->url, "https://example.com/mcp");
    EXPECT_EQ(config->headers.at("Authorization"), "Bearer ${T}");
    EXPECT_EQ(*config->authProvider, "anthropic");
}

TEST_F(McpServerConfigValidatorTest, RejectsBadNamesAndShapes) {
    EXPECT_EQ(failure("bad name", Json::object()),
              "invalid server name \"bad name\" (use letters, digits, \"_\" and \"-\")");
    EXPECT_EQ(failure("a", Json::array()), "server \"a\" must be an object");
    EXPECT_EQ(failure("a", Json{{"args", Json::array()}}),
              "server \"a\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)");
    EXPECT_NE(failure("a", Json{{"type", "sse"}, {"url", "https://x"}}).find("legacy SSE"), std::string::npos);
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"type", "http"}}),
              "server \"a\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)");
}

TEST_F(McpServerConfigValidatorTest, RejectsBadFieldTypes) {
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"args", Json::array({1})}}),
              "server \"a\": args must be an array of strings");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"env", Json{{"K", 1}}}}),
              "server \"a\": env must map names to strings");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"cwd", 1}}), "server \"a\": cwd must be a string");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"enabled", "yes"}}),
              "server \"a\": enabled must be a boolean");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"timeout", 0}}),
              "server \"a\": timeout must be a positive number of seconds");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"description", 1}}),
              "server \"a\": description must be a string");
    EXPECT_EQ(failure("a", Json{{"url", "ftp://x"}}), "server \"a\": url must be an http or https URL");
    EXPECT_EQ(failure("a", Json{{"url", "https://x"}, {"headers", Json{{"A", 1}}}}),
              "server \"a\": headers must map names to strings");
    EXPECT_EQ(failure("a", Json{{"url", "https://x"}, {"oauth", 1}}), "server \"a\": oauth must be an object");
}

TEST_F(McpServerConfigValidatorTest, AuthProviderNeedsSecureUrl) {
    EXPECT_EQ(failure("a", Json{{"url", "http://example.com/mcp"}, {"auth", Json{{"provider", "p"}}}}),
              "server \"a\": auth requires an https URL, or http on localhost, 127.0.0.1, or [::1]");
    EXPECT_TRUE(m_validator.validate("a", Json{{"url", "http://localhost:8080/mcp"}, {"auth", Json{{"provider", "p"}}}})
                    .has_value());
    EXPECT_TRUE(m_validator.validate("a", Json{{"url", "http://[::1]:8080/mcp"}, {"auth", Json{{"provider", "p"}}}})
                    .has_value());
    EXPECT_EQ(failure("a", Json{{"url", "https://x"}, {"auth", Json{{"provider", ""}}}}),
              "server \"a\": auth.provider must be a provider name");
}

TEST_F(McpServerConfigValidatorTest, ExposureAliasesAndErrors) {
    const auto config = m_validator.validate(
        "a", Json{{"command", "x"}, {"exposure", "codemode-deferred"},
                  {"toolExposure", Json{{"t", "codemode-deferred"}, {"u", "direct"}}}});
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->exposure, McpExposure::Codemode);
    EXPECT_EQ(config->raw["exposure"], "codemode");
    EXPECT_EQ(config->toolExposure.size(), 2U);
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"exposure", "nope"}}),
              "server \"a\": exposure must be one of \"codemode\", \"deferred\", \"direct\", \"hidden\"");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"toolExposure", Json{{"t", "bad"}}}}),
              "server \"a\": toolExposure \"t\" must be one of \"codemode\", \"deferred\", \"direct\", \"hidden\"");
    EXPECT_EQ(failure("a", Json{{"command", "x"}, {"toolExposure", 1}}),
              "server \"a\": toolExposure must map tool names to exposures");
}

TEST_F(McpServerConfigValidatorTest, ToolExposurePrefersExactNamesThenFirstPattern) {
    const auto config = m_validator.validate(
        "a", Json{{"command", "x"}, {"exposure", "hidden"},
                  {"toolExposure", Json{{"read_*", "direct"}, {"*", "deferred"}, {"read_file", "codemode"}}}});
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(m_validator.toolExposure(*config, "read_file"), McpExposure::Codemode);
    EXPECT_EQ(m_validator.toolExposure(*config, "read_dir"), McpExposure::Direct);
    EXPECT_EQ(m_validator.toolExposure(*config, "write"), McpExposure::Deferred);
    const auto plain = m_validator.validate("b", Json{{"command", "x"}, {"exposure", "hidden"}});
    EXPECT_EQ(m_validator.toolExposure(*plain, "anything"), McpExposure::Hidden);
}

TEST_F(McpServerConfigValidatorTest, NamespaceReplacesDashes) {
    EXPECT_EQ(m_validator.namespaceOf("my-server"), "mcp__my_server");
}
