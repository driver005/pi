#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.google_adc_auth;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class GoogleAdcAuthTest : public testing::Test {
protected:
    GoogleAdcAuthTest() {
        m_files.createDirectories("/home/user/.config/gcloud");
        m_files.createDirectories("/keys");
    }

    std::string privateKey() {
        return R"(-----BEGIN PRIVATE KEY-----
MIICeAIBADANBgkqhkiG9w0BAQEFAASCAmIwggJeAgEAAoGBANRLNx6mqBUz4oe5
LkkKXXSUbbd3LbU1dpIFW9I7NR8jWVBP9S2AYsg2VgTWaiEpZdxcFRagBZccb1my
klsArUtcK+RpBZ1jJtTSQFbwWd1kGNMgLJM5ZFOv6Xkgt8LBnUr06qxm2UCAGEvW
w7Gayy89YxMplwqY+vsTMsTuVFonAgMBAAECgYEAmB045paF04N06slOl/l8U19T
amVT9AbV6fU7AN15x9D9WyWfyTW4EjuU0SyNqStPmGDGn4qT1t4CD2R7qAdJI2sj
KLECQv69yD0XSOLqcioeOUMAlgB4/nOeve2wyb9qYiWB1vHm9t8//Pnv4e3pTyMU
DsZjdJUS4R0lW1jgY0kCQQDxY2nQ4Rk8gpAtVbYU9cJF5wsMb+5aVLZvMgRU5OeF
5kTQpfBPKzZpXi2cXVzUEtZQCnHRk7U2OpCbpPDITGl9AkEA4STzAe41LJL+A7HO
i6NnUoFr36G1NDK2O+V5sHghChgWBc4jke6BYPTO8kmGnDDvinQqueqZll62QSfe
8P6DcwJBANnyvecIZ0XYSR91xTp1j1yYOMSZB6ft1u7dRUX1jAm9GKMfQLPqu201
yI7nSVp+S5znYU8uQ67cABdYPMNNIu0CQFoP1cWn7E1wX3xK3DyvmN1AOE60+S9w
OcWr/gnBhDXtfKHF3CS8K7UFOONi1h4U1T2lSpIkblvgdzeJI31z0lECQQDhvarD
YUlbP5V2vpZ6mdnBPeD0iTUJpZBaq/RoxSKuuFbTJVnzEfs6XBQkO8EwmgSJIQfd
2/lrNVTIFy3LJDqm
-----END PRIVATE KEY-----
)";
    }

    HttpResponse tokenReply(const std::string& token, int expiresIn = 3600) {
        HttpResponse response;
        response.status = 200;
        response.body = Json{{"access_token", token}, {"expires_in", expiresIn}}.dump();
        return response;
    }

    std::map<std::string, std::string> formOf(const std::string& body) {
        std::map<std::string, std::string> out;
        std::size_t start = 0;
        while (start < body.size()) {
            std::size_t end = body.find('&', start);
            end = end == std::string::npos ? body.size() : end;
            const std::string pair = body.substr(start, end - start);
            const auto equals = pair.find('=');
            out[pair.substr(0, equals)] = pair.substr(equals + 1);
            start = end + 1;
        }
        return out;
    }

    FakeFileSystem m_files;
    FakeEnvironment m_environment;
    FixedClock m_clock{1'700'000'000'000};
    ScriptedHttpClient m_http;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    GoogleAdcAuth m_auth{m_http, m_files, m_environment, m_clock, m_crypto, m_base64};
};

TEST_F(GoogleAdcAuthTest, ServiceAccountKeySignsAJwtAndExchangesIt) {
    m_files.writeFile("/keys/sa.json", Json{{"type", "service_account"},
                                             {"client_email", "bot@project.iam.gserviceaccount.com"},
                                             {"private_key", privateKey()},
                                             {"token_uri", "https://oauth2.example.com/token"}}
                                           .dump());
    m_environment.set("GOOGLE_APPLICATION_CREDENTIALS", "/keys/sa.json");
    m_http.enqueue(tokenReply("ya29.token"));
    const auto token = m_auth.token({});
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "ya29.token");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://oauth2.example.com/token");
    const auto form = formOf(request.body);
    EXPECT_EQ(form.at("grant_type"), "urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer");
    const std::string jwt = form.at("assertion");
    const auto firstDot = jwt.find('.');
    const auto secondDot = jwt.find('.', firstDot + 1);
    ASSERT_NE(secondDot, std::string::npos);
    const Json header = Json::parse(*m_base64.decode(jwt.substr(0, firstDot)));
    const Json claims = Json::parse(*m_base64.decode(jwt.substr(firstDot + 1, secondDot - firstDot - 1)));
    EXPECT_EQ(header, (Json{{"alg", "RS256"}, {"typ", "JWT"}}));
    EXPECT_EQ(claims["iss"], "bot@project.iam.gserviceaccount.com");
    EXPECT_EQ(claims["aud"], "https://oauth2.example.com/token");
    EXPECT_EQ(claims["scope"], "https://www.googleapis.com/auth/cloud-platform");
    EXPECT_EQ(claims["exp"].get<std::int64_t>() - claims["iat"].get<std::int64_t>(), 3600);
    const auto expected = m_crypto.rsaSha256Sign(privateKey(), jwt.substr(0, secondDot));
    EXPECT_EQ(*m_base64.decode(jwt.substr(secondDot + 1)), *expected);
}

TEST_F(GoogleAdcAuthTest, AuthorizedUserRefreshesFromTheWellKnownFile) {
    m_files.writeFile("/home/user/.config/gcloud/application_default_credentials.json",
                      Json{{"type", "authorized_user"},
                           {"client_id", "id"},
                           {"client_secret", "se cret"},
                           {"refresh_token", "r/t"}}
                          .dump());
    m_http.enqueue(tokenReply("user-token"));
    EXPECT_EQ(*m_auth.token({}), "user-token");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://oauth2.googleapis.com/token");
    const auto form = formOf(request.body);
    EXPECT_EQ(form.at("grant_type"), "refresh_token");
    EXPECT_EQ(form.at("client_secret"), "se%20cret");
    EXPECT_EQ(form.at("refresh_token"), "r%2Ft");
}

TEST_F(GoogleAdcAuthTest, MetadataServerIsTheFallback) {
    m_http.enqueue(tokenReply("meta-token"));
    EXPECT_EQ(*m_auth.token({}), "meta-token");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.method, "GET");
    EXPECT_NE(request.url.find("metadata.google.internal"), std::string::npos);
    EXPECT_EQ(request.headers[0].second, "Google");
}

TEST_F(GoogleAdcAuthTest, TokensAreCachedUntilNearExpiry) {
    m_http.enqueue(tokenReply("first", 3600));
    m_http.enqueue(tokenReply("second", 3600));
    EXPECT_EQ(*m_auth.token({}), "first");
    m_clock.advance(30 * 60 * 1000);
    EXPECT_EQ(*m_auth.token({}), "first");
    EXPECT_EQ(m_http.calls(), 1);
    m_clock.advance(30 * 60 * 1000);
    EXPECT_EQ(*m_auth.token({}), "second");
    EXPECT_EQ(m_http.calls(), 2);
}

TEST_F(GoogleAdcAuthTest, ProviderEnvironmentSelectsTheKeyFile) {
    m_files.writeFile("/keys/other.json", Json{{"type", "authorized_user"}, {"refresh_token", "x"}}.dump());
    m_http.enqueue(tokenReply("scoped"));
    EXPECT_EQ(*m_auth.token({{"GOOGLE_APPLICATION_CREDENTIALS", "/keys/other.json"}}), "scoped");
}

TEST_F(GoogleAdcAuthTest, FailuresAreReported) {
    HttpResponse denied;
    denied.status = 400;
    denied.body = R"({"error":"invalid_grant"})";
    m_files.writeFile("/keys/u.json", Json{{"type", "authorized_user"}}.dump());
    m_http.enqueue(denied);
    const auto rejected = m_auth.token({{"GOOGLE_APPLICATION_CREDENTIALS", "/keys/u.json"}});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message.find("invalid_grant"), std::string::npos);

    m_files.writeFile("/keys/bad.json", R"({"type":"external_account"})");
    const auto unsupported = m_auth.token({{"GOOGLE_APPLICATION_CREDENTIALS", "/keys/bad.json"}});
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_NE(unsupported.error().message.find("Unsupported"), std::string::npos);

    EXPECT_FALSE(m_auth.token({}).has_value());
}
