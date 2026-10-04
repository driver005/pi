#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.credential_codec;

class CredentialCodecTest : public testing::Test {
protected:
    CredentialCodec m_codec;
};

TEST_F(CredentialCodecTest, ApiKeyRoundTripKeepsUnknownFields) {
    const Json json = Json::parse(R"({"type":"api_key","key":"!pass x","env":{"A":"1"},"note":"keep"})");
    const auto credential = m_codec.fromJson("anthropic", json);
    ASSERT_TRUE(credential.has_value());
    EXPECT_EQ(credential->type, CredentialType::ApiKey);
    EXPECT_EQ(credential->key, "!pass x");
    EXPECT_EQ(credential->env->at("A"), "1");
    EXPECT_EQ(m_codec.toJson(*credential), json);
}

TEST_F(CredentialCodecTest, OAuthRoundTrip) {
    const Json json = Json::parse(R"({"type":"oauth","refresh":"r","access":"a","expires":1700000000000,"accountId":"x"})");
    const auto credential = m_codec.fromJson("openai-codex", json);
    ASSERT_TRUE(credential.has_value());
    EXPECT_EQ(credential->access, "a");
    EXPECT_EQ(credential->refresh, "r");
    EXPECT_EQ(credential->expires, 1700000000000.0);
    EXPECT_EQ(credential->extra["accountId"], "x");
    EXPECT_EQ(m_codec.toJson(*credential)["expires"], 1700000000000LL);
    EXPECT_EQ(m_codec.toJson(*credential)["accountId"], "x");
}

TEST_F(CredentialCodecTest, RejectsMalformedEntries) {
    for (const char* text : {R"([])", R"({"type":"api_key","key":1})", R"({"type":"api_key","env":{"A":1}})",
                             R"({"type":"oauth","access":"a"})", R"({"type":"magic"})", R"({})"}) {
        EXPECT_FALSE(m_codec.fromJson("p", Json::parse(text)).has_value()) << text;
    }
}

TEST_F(CredentialCodecTest, ParsesAndWritesDocuments) {
    const std::string text = "\xEF\xBB\xBF{\"a\":{\"type\":\"api_key\",\"key\":\"k\"},\"b\":{\"type\":\"oauth\",\"access\":\"x\",\"refresh\":\"y\",\"expires\":5}}";
    const auto entries = m_codec.parseDocument(text);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2U);
    EXPECT_EQ((*entries)[0].first, "a");
    const std::string out = m_codec.serializeDocument(*entries);
    EXPECT_EQ(Json::parse(out)["b"]["expires"], 5);
    EXPECT_NE(out.find("\n  \"a\""), std::string::npos);
}

TEST_F(CredentialCodecTest, EmptyDocumentIsEmpty) {
    EXPECT_TRUE(m_codec.parseDocument("")->empty());
    EXPECT_TRUE(m_codec.parseDocument("  \n")->empty());
    EXPECT_FALSE(m_codec.parseDocument("[1]").has_value());
    EXPECT_FALSE(m_codec.parseDocument("{broken").has_value());
}
