#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.plugin_oauth_flow;

class PluginOauthFlowTest : public testing::Test {
protected:
    PluginOauthFlow make(PluginOauthFlow::Login login, PluginOauthFlow::Refresh refresh) {
        return PluginOauthFlow("acme", "Acme", true, std::move(login), std::move(refresh));
    }

    PluginOauthFlow::Refresh noRefresh() {
        return [](const Json&, const std::shared_ptr<AbortSignal>&) -> Result<Json> { return Json{{"error", "no refresh"}}; };
    }
};

TEST_F(PluginOauthFlowTest, LoginReturnsTheCredentialWithItsExtraFields) {
    LoginInteraction interaction;
    std::string shown;
    interaction.authUrl = [&shown](const std::string& url, const std::string&) { shown = url; };
    PluginOauthFlow flow = make(
        [](const LoginInteraction& ui) -> Result<Json> {
            ui.authUrl("https://acme.test/login", "open it");
            return Json{{"access", "a1"}, {"refresh", "r1"}, {"expires", 5000}, {"accountId", "acct"}};
        },
        noRefresh());
    EXPECT_EQ(flow.providerId(), "acme");
    EXPECT_EQ(flow.name(), "Acme");
    EXPECT_TRUE(flow.isSubscription());
    const auto credential = flow.login(interaction);
    ASSERT_TRUE(credential.has_value());
    EXPECT_EQ(shown, "https://acme.test/login");
    EXPECT_EQ(credential->type, CredentialType::OAuth);
    EXPECT_EQ(credential->access, "a1");
    EXPECT_EQ(credential->refresh, "r1");
    EXPECT_EQ(credential->expires, 5000);
    EXPECT_EQ(credential->extra["accountId"], "acct");
    EXPECT_EQ(flow.toAuth(*credential).apiKey, "a1");
}

TEST_F(PluginOauthFlowTest, RefreshPassesTheStoredCredentialBack) {
    Json seen;
    PluginOauthFlow flow = make(
        [](const LoginInteraction&) -> Result<Json> { return Json{{"error", "unused"}}; },
        [&seen](const Json& credential, const std::shared_ptr<AbortSignal>&) -> Result<Json> {
            seen = credential;
            return Json{{"access", "a2"}, {"refresh", "r2"}, {"expires", 9000}, {"accountId", credential["accountId"]}};
        });
    Credential stored;
    stored.type = CredentialType::OAuth;
    stored.access = "a1";
    stored.refresh = "r1";
    stored.expires = 5000;
    stored.extra["accountId"] = "acct";
    const auto refreshed = flow.refresh(stored, nullptr);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ(seen["refresh"], "r1");
    EXPECT_EQ(seen["accountId"], "acct");
    EXPECT_EQ(refreshed->access, "a2");
    EXPECT_EQ(refreshed->extra["accountId"], "acct");
}

TEST_F(PluginOauthFlowTest, FailuresAndMalformedAnswersAreErrors) {
    PluginOauthFlow failing = make([](const LoginInteraction&) -> Result<Json> { return Json{{"error", "denied"}}; }, noRefresh());
    const auto denied = failing.login(LoginInteraction{});
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().message, "denied");

    PluginOauthFlow incomplete = make([](const LoginInteraction&) -> Result<Json> { return Json{{"access", "a"}}; }, noRefresh());
    EXPECT_FALSE(incomplete.login(LoginInteraction{}).has_value());

    PluginOauthFlow notObject = make([](const LoginInteraction&) -> Result<Json> { return Json(3); }, noRefresh());
    EXPECT_FALSE(notObject.login(LoginInteraction{}).has_value());

    PluginOauthFlow errored = make([](const LoginInteraction&) -> Result<Json> { return std::unexpected(Error{"x", "boom"}); }, noRefresh());
    EXPECT_EQ(errored.login(LoginInteraction{}).error().message, "boom");
}

TEST_F(PluginOauthFlowTest, CloseWaitsForRunningCallsAndRefusesLaterOnes) {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    PluginOauthFlow flow = make(
        [&](const LoginInteraction&) -> Result<Json> {
            entered.set_value();
            gate.wait();
            return Json{{"access", "a"}, {"refresh", "r"}, {"expires", 1}};
        },
        noRefresh());
    std::thread caller([&flow] { EXPECT_TRUE(flow.login(LoginInteraction{}).has_value()); });
    entered.get_future().wait();
    std::atomic<bool> closed{false};
    std::thread closer([&] {
        flow.close();
        closed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(closed.load()) << "close waits for the login in progress";
    release.set_value();
    caller.join();
    closer.join();
    EXPECT_TRUE(closed.load());
    const auto late = flow.login(LoginInteraction{});
    ASSERT_FALSE(late.has_value());
    EXPECT_NE(late.error().message.find("no longer loaded"), std::string::npos);
}
