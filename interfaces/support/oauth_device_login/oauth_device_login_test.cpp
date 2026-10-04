#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.oauth_device_login;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class AdvancingSleeper : public ISleeper {
public:
    explicit AdvancingSleeper(FixedClock& clock)
        : m_clock(clock) {}

    bool sleep(std::chrono::milliseconds duration, const std::shared_ptr<AbortSignal>& signal) override {
        m_clock.advance(duration.count());
        return !(signal && signal->aborted());
    }

private:
    FixedClock& m_clock;
};

class OauthDeviceLoginTest : public testing::Test {
protected:
    OauthDeviceLoginTest() {
        m_spec.name = "Acme";
        m_spec.deviceUrl = "https://auth.acme.test/device";
        m_spec.tokenUrl = "https://auth.acme.test/token";
        m_spec.clientId = "client-1";
        m_spec.scope = "read write";
        m_spec.defaultExpiresSeconds = 900;
        m_interaction.deviceCode = [this](const std::string& code, const std::string& uri, std::optional<std::int64_t> interval, std::optional<std::int64_t> expires) {
            m_shown = {code, uri, std::to_string(interval.value_or(-1)), std::to_string(expires.value_or(-1))};
        };
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Json deviceAnswer() {
        return Json{{"device_code", "dev-1"}, {"user_code", "ABCD-EFGH"}, {"verification_uri", "https://acme.test/device"}, {"verification_uri_complete", "https://acme.test/device?code=ABCD-EFGH"}, {"interval", 2}, {"expires_in", 600}};
    }

    DeviceLoginSpec m_spec;
    LoginInteraction m_interaction;
    std::vector<std::string> m_shown;
    ScriptedHttpClient m_http;
    FixedClock m_clock{1'000'000};
    AdvancingSleeper m_sleeper{m_clock};
    OauthDevicePoller m_poller{m_clock, m_sleeper};
    OauthDeviceLogin m_login{m_http, m_poller};
};

TEST_F(OauthDeviceLoginTest, ShowsTheCodeAndReturnsTheTokensOnceTheUserApproved) {
    m_http.enqueue(reply(deviceAnswer()));
    m_http.enqueue(reply(Json{{"error", "authorization_pending"}}, 400));
    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    const auto result = m_login.login(m_spec, m_interaction);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)["access_token"], "tok");
    EXPECT_EQ(m_shown, (std::vector<std::string>{"ABCD-EFGH", "https://acme.test/device", "2", "600"}));
    const auto requests = m_http.requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[0].url, "https://auth.acme.test/device");
    EXPECT_EQ(requests[0].body, "client_id=client-1&scope=read%20write");
    EXPECT_EQ(requests[1].body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=client-1&device_code=dev-1");
}

TEST_F(OauthDeviceLoginTest, TheCompleteUriIsShownWhenPreferred) {
    m_spec.preferCompleteUri = true;
    m_http.enqueue(reply(deviceAnswer()));
    m_http.enqueue(reply(Json{{"access_token", "tok"}}));
    ASSERT_TRUE(m_login.login(m_spec, m_interaction).has_value());
    EXPECT_EQ(m_shown[1], "https://acme.test/device?code=ABCD-EFGH");
}

TEST_F(OauthDeviceLoginTest, ExtraDeviceParametersAndTheDeviceCodeFieldAreSent) {
    m_spec.deviceParams = {{"referrer", "pi"}};
    m_spec.deviceCodeField = "code";
    m_http.enqueue(reply(deviceAnswer()));
    m_http.enqueue(reply(Json{{"access_token", "tok"}}));
    ASSERT_TRUE(m_login.login(m_spec, m_interaction).has_value());
    const auto requests = m_http.requests();
    EXPECT_NE(requests[0].body.find("referrer=pi"), std::string::npos);
    EXPECT_NE(requests[1].body.find("&code=dev-1"), std::string::npos);
}

TEST_F(OauthDeviceLoginTest, DefaultsFillMissingIntervalAndExpiryAndAnExpiryCanBeRequired) {
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "u"}, {"verification_uri", "https://acme.test/d"}}));
    m_http.enqueue(reply(Json{{"access_token", "tok"}}));
    ASSERT_TRUE(m_login.login(m_spec, m_interaction).has_value());
    EXPECT_EQ(m_shown[2], "5");
    EXPECT_EQ(m_shown[3], "900");

    m_spec.defaultExpiresSeconds = 0;
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "u"}, {"verification_uri", "https://acme.test/d"}}));
    const auto missing = m_login.login(m_spec, m_interaction);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find("expires_in"), std::string::npos);
}

TEST_F(OauthDeviceLoginTest, UntrustedAndIncompleteResponsesAreRefused) {
    m_spec.httpsOnly = true;
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "u"}, {"verification_uri", "http://acme.test/d"}, {"expires_in", 60}}));
    auto result = m_login.login(m_spec, m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Untrusted verification URI in Acme OAuth response");
    m_spec.httpsOnly = false;
    m_spec.requireCompleteUri = true;
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "u"}, {"verification_uri", "https://acme.test/d"}, {"expires_in", 60}}));
    result = m_login.login(m_spec, m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("Invalid Acme device authorization response"), std::string::npos);
    m_http.enqueue(reply(Json{{"error", "invalid_client"}, {"error_description", "bad client"}}, 400));
    result = m_login.login(m_spec, m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Acme device authorization failed with status 400: bad client");
}

TEST_F(OauthDeviceLoginTest, DenialExpiryAndServerErrorsEndThePolling) {
    const std::vector<std::pair<Json, std::string>> cases = {{Json{{"error", "access_denied"}}, "Acme login was denied."},
                                                             {Json{{"error", "expired_token"}}, "Acme device authorization expired. Please restart login."},
                                                             {Json{{"error", "boom"}, {"error_description", "why"}}, "Acme device token request failed (status 400): boom: why"}};
    for (const auto& [body, message] : cases) {
        m_http.enqueue(reply(deviceAnswer()));
        m_http.enqueue(reply(body, 400));
        const auto result = m_login.login(m_spec, m_interaction);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().message, message);
    }
    m_spec.serverErrorFails = true;
    m_http.enqueue(reply(deviceAnswer()));
    m_http.enqueue(reply(Json{{"error", "x"}}, 503));
    const auto result = m_login.login(m_spec, m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("status 503"), std::string::npos);
}

TEST_F(OauthDeviceLoginTest, SlowDownRaisesTheIntervalBeforeThePollSucceeds) {
    m_http.enqueue(reply(deviceAnswer()));
    m_http.enqueue(reply(Json{{"error", "slow_down"}}, 400));
    m_http.enqueue(reply(Json{{"access_token", "tok"}}));
    const std::int64_t start = m_clock.nowMs();
    ASSERT_TRUE(m_login.login(m_spec, m_interaction).has_value());
    // waits the interval before the first poll (2 s), then the raised one (7 s)
    EXPECT_EQ(m_clock.nowMs() - start, 9000);
}
