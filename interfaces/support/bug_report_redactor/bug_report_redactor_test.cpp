#include <gtest/gtest.h>

import std;
import pi.support.bug_report_redactor;

class BugReportRedactorTest : public testing::Test {
protected:
    BugReportRedactor m_redactor;
};

TEST_F(BugReportRedactorTest, SecretWordsAreSensitiveKeysInAnyCase) {
    for (const std::string key : {"apiKey", "api_key", "api-key", "API_KEY", "token", "accessToken", "client_secret", "Password", "authorization", "cookie", "x-auth-token", "credential"}) {
        EXPECT_TRUE(m_redactor.isSensitiveKey(key)) << key;
    }
    for (const std::string key : {"name", "tokens", "maxTokens", "monkey", "passwordless", "model"}) {
        EXPECT_FALSE(m_redactor.isSensitiveKey(key)) << key;
    }
}

TEST_F(BugReportRedactorTest, UrlsLoseCredentialsAndSecretQueryValues) {
    EXPECT_EQ(m_redactor.redactUrl("https://user:pw@example.com/v1?key=1&api_key=abc&x=2#frag"), "https://example.com/v1?key=1&api_key=%3Credacted%3E&x=2#frag");
    EXPECT_EQ(m_redactor.redactUrl("https://example.com/v1?x=2"), "https://example.com/v1?x=2");
    EXPECT_EQ(m_redactor.redactUrl("git+https://tok@github.com/a/b"), "git+https://github.com/a/b");
    EXPECT_EQ(m_redactor.redactUrl("npm:left-pad"), "npm:left-pad");
    EXPECT_EQ(m_redactor.redactUrl("not a url"), "not a url");
    EXPECT_EQ(m_redactor.redactUrl("https://example.com/?Token=1"), "https://example.com/?Token=%3Credacted%3E");
}

TEST_F(BugReportRedactorTest, JsonValuesUnderSecretKeysAreReplacedRecursively) {
    const Json input = Json::parse(R"({"model":"m","apiKey":"sk-123","nested":{"authToken":"t","list":[{"password":"p","url":"https://u:p@h.io/x"}]},"headers":{"Authorization":"Bearer x","accept":"json"},"n":null,"token":null})");
    const Json out = m_redactor.redactJson(input);
    EXPECT_EQ(out["apiKey"], "<redacted>");
    EXPECT_EQ(out["nested"]["authToken"], "<redacted>");
    EXPECT_EQ(out["nested"]["list"][0]["password"], "<redacted>");
    EXPECT_EQ(out["nested"]["list"][0]["url"], "https://h.io/x");
    EXPECT_EQ(out["headers"]["Authorization"], "<redacted>");
    EXPECT_EQ(out["headers"]["accept"], "json");
    EXPECT_EQ(out["model"], "m");
    EXPECT_TRUE(out["token"].is_null()) << "null stays null";
}
