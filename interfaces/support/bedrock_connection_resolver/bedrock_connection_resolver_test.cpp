#include <gtest/gtest.h>

import std;
import pi.support.bedrock_connection_resolver;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class BedrockConnectionResolverTest : public testing::Test {
protected:
    BedrockConnectionResolverTest() {
        m_files.createDirectories("/home/user/.aws");
    }

    Model model(const std::string& baseUrl = "https://bedrock-runtime.us-east-1.amazonaws.com",
                const std::string& id = "anthropic.claude-v2") {
        Model m;
        m.id = id;
        m.baseUrl = baseUrl;
        return m;
    }

    FakeFileSystem m_files;
    FakeEnvironment m_environment;
    BedrockConnectionResolver m_resolver{m_environment, m_files};
};

TEST_F(BedrockConnectionResolverTest, EnvironmentKeysWithConfiguredRegion) {
    m_environment.set("AWS_ACCESS_KEY_ID", "AKID");
    m_environment.set("AWS_SECRET_ACCESS_KEY", "secret");
    m_environment.set("AWS_SESSION_TOKEN", "token");
    m_environment.set("AWS_REGION", "eu-central-1");
    const auto connection = m_resolver.resolve(model(), std::nullopt, {});
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->region, "eu-central-1");
    EXPECT_EQ(connection->endpoint, "https://bedrock-runtime.eu-central-1.amazonaws.com");
    ASSERT_TRUE(connection->credentials.has_value());
    EXPECT_EQ(connection->credentials->accessKeyId, "AKID");
    EXPECT_EQ(connection->credentials->sessionToken, "token");
    EXPECT_FALSE(connection->bearerToken.has_value());
}

TEST_F(BedrockConnectionResolverTest, BearerTokenFromApiKeyOrEnvironment) {
    const auto fromKey = m_resolver.resolve(model(), std::string("bk"), {});
    EXPECT_EQ(fromKey->bearerToken, "bk");
    EXPECT_FALSE(fromKey->credentials.has_value());
    m_environment.set("AWS_BEARER_TOKEN_BEDROCK", "envtoken");
    EXPECT_EQ(m_resolver.resolve(model(), std::nullopt, {})->bearerToken, "envtoken");
    EXPECT_EQ(m_resolver.resolve(model(), std::nullopt, {{"AWS_BEARER_TOKEN_BEDROCK", "scoped"}})->bearerToken, "scoped");
}

TEST_F(BedrockConnectionResolverTest, SkipAuthUsesDummyCredentials) {
    const auto connection = m_resolver.resolve(model("https://proxy.example.com/bedrock/"), std::string("ignored"),
                                               {{"AWS_BEDROCK_SKIP_AUTH", "1"}});
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->endpoint, "https://proxy.example.com/bedrock");
    EXPECT_EQ(connection->credentials->accessKeyId, "dummy-access-key");
    EXPECT_FALSE(connection->bearerToken.has_value());
}

TEST_F(BedrockConnectionResolverTest, ProfileCredentialsAndRegionFromFiles) {
    m_files.writeFile("/home/user/.aws/credentials",
                      "[dev]\naws_access_key_id = DEVKEY\naws_secret_access_key = DEVSECRET\naws_session_token = DEVTOKEN\n");
    m_files.writeFile("/home/user/.aws/config", "[profile dev]\nregion = ap-southeast-2\n");
    m_environment.set("AWS_ACCESS_KEY_ID", "ambient");
    m_environment.set("AWS_SECRET_ACCESS_KEY", "ambient");
    const auto connection = m_resolver.resolve(model(), std::nullopt, {{"AWS_PROFILE", "dev"}});
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->credentials->accessKeyId, "DEVKEY");
    EXPECT_EQ(connection->credentials->sessionToken, "DEVTOKEN");
    EXPECT_EQ(connection->region, "ap-southeast-2");
}

TEST_F(BedrockConnectionResolverTest, DefaultProfileIsTheFallback) {
    m_files.writeFile("/home/user/.aws/credentials", "[default]\naws_access_key_id = K\naws_secret_access_key = S\n");
    const auto connection = m_resolver.resolve(model(), std::nullopt, {});
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->credentials->accessKeyId, "K");
    EXPECT_EQ(connection->region, "us-east-1");
}

TEST_F(BedrockConnectionResolverTest, MissingCredentialsAreReported) {
    const auto connection = m_resolver.resolve(model(), std::nullopt, {});
    ASSERT_FALSE(connection.has_value());
    EXPECT_NE(connection.error().message.find("AWS_BEARER_TOKEN_BEDROCK"), std::string::npos);
}

TEST_F(BedrockConnectionResolverTest, ArnRegionWinsAndCustomEndpointsAreKept) {
    m_environment.set("AWS_ACCESS_KEY_ID", "K");
    m_environment.set("AWS_SECRET_ACCESS_KEY", "S");
    m_environment.set("AWS_REGION", "us-west-2");
    const auto arn = m_resolver.resolve(
        model("https://vpce.example.com", "arn:aws:bedrock:eu-west-3:123456789012:inference-profile/x"), std::nullopt, {});
    EXPECT_EQ(arn->region, "eu-west-3");
    EXPECT_EQ(arn->endpoint, "https://vpce.example.com");
}

TEST_F(BedrockConnectionResolverTest, StandardEndpointRegionIsUsedWhenNothingIsConfigured) {
    m_environment.set("AWS_ACCESS_KEY_ID", "K");
    m_environment.set("AWS_SECRET_ACCESS_KEY", "S");
    const auto connection = m_resolver.resolve(model("https://bedrock-runtime.eu-north-1.amazonaws.com"), std::nullopt, {});
    EXPECT_EQ(connection->region, "eu-north-1");
    EXPECT_EQ(connection->endpoint, "https://bedrock-runtime.eu-north-1.amazonaws.com");
}
