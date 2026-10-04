#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.auth_command;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class FakeCallbackServer : public ICallbackServer {
public:
    Result<int> listen(const std::string&, int port, bool) override {
        return port != 0 ? port : 45678;
    }
    Result<Json> waitForCallback(const std::vector<std::string>&, const std::string& state, std::chrono::milliseconds, const std::shared_ptr<AbortSignal>&) override {
        return Json{{"code", "the-code"}, {"state", state}};
    }
    void close() override {}
};

class AuthCommandTest : public testing::Test {
protected:
    AuthCommandTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/auth_command_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/agent");
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    CommandLine line(const std::vector<std::string>& arguments, const std::string& method = "", bool manual = false) {
        CommandLine result;
        result.command = "auth";
        result.arguments = arguments;
        result.loginMethod = method;
        result.loginManual = manual;
        result.options.agentDir = m_dir + "/agent";
        result.options.startup.pluginPaths = m_pluginPaths;
        return result;
    }

    int run(const std::vector<std::string>& arguments, const std::string& method = "", bool manual = false) {
        AuthCommand command(*m_services, m_http, m_sleeper, []() { return std::unique_ptr<ICallbackServer>(std::make_unique<FakeCallbackServer>()); }, m_in, m_out, m_err);
        return command.run(line(arguments, method, manual));
    }

    HttpResponse reply(int status, const Json& body) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Json authFile() {
        return Json::parse(std::ifstream(m_dir + "/agent/auth.json"));
    }

    std::string m_dir;
    std::vector<std::string> m_pluginPaths;
    std::unique_ptr<CodingServices> m_services;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    std::istringstream m_in;
    std::ostringstream m_out;
    std::ostringstream m_err;
};

TEST_F(AuthCommandTest, ListShowsTheProvidersAndTheirMethods) {
    EXPECT_EQ(run({"list"}), 0);
    EXPECT_NE(m_out.str().find("anthropic\tbrowser, copy_code\n"), std::string::npos);
    EXPECT_NE(m_out.str().find("xai\tdevice_code\n"), std::string::npos);
}

TEST_F(AuthCommandTest, LoginPrintsTheUrlStoresTheCredentialAndStatusShowsIt) {
    m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    EXPECT_EQ(run({"login", "anthropic"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("https://claude.ai/oauth/authorize?"), std::string::npos);
    EXPECT_NE(m_out.str().find("Signed in to \"anthropic\"."), std::string::npos);
    const Json file = authFile();
    ASSERT_TRUE(file.contains("anthropic"));
    EXPECT_EQ(file["anthropic"]["type"], "oauth");
    EXPECT_EQ(file["anthropic"]["access"], "tok");
    EXPECT_EQ(file["anthropic"]["refresh"], "ref");

    m_out.str("");
    EXPECT_EQ(run({"status"}), 0);
    EXPECT_NE(m_out.str().find("anthropic\tOAuth (access token valid)"), std::string::npos);
}

TEST_F(AuthCommandTest, ManualLoginAsksForThePastedCode) {
    m_in.str("pasted-code#whatever\n");
    m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    // The pasted state is not the sign-in's, so it is refused.
    EXPECT_EQ(run({"login", "anthropic"}, "copy_code"), 1);
    EXPECT_NE(m_err.str().find("OAuth state mismatch"), std::string::npos);
    EXPECT_NE(m_out.str().find("paste"), std::string::npos);
}

TEST_F(AuthCommandTest, DeviceLoginShowsTheCodeAndStoresTheCredential) {
    m_http.enqueue(reply(200, Json{{"device_code", "d"}, {"user_code", "XAI-1"}, {"verification_uri", "https://x.ai/device"}, {"expires_in", 600}, {"interval", 1}}));
    m_http.enqueue(reply(200, Json{{"access_token", "xtok"}, {"refresh_token", "xref"}, {"expires_in", 3600}}));
    EXPECT_EQ(run({"login", "xai"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("XAI-1"), std::string::npos);
    EXPECT_NE(m_out.str().find("https://x.ai/device"), std::string::npos);
    EXPECT_EQ(authFile()["xai"]["access"], "xtok");
}

TEST_F(AuthCommandTest, FailuresAreReportedWithAnExitCode) {
    EXPECT_EQ(run({"login", "nobody"}), 1);
    EXPECT_NE(m_err.str().find("No sign-in is available"), std::string::npos);
    m_err.str("");
    EXPECT_EQ(run({"login", "anthropic"}, "device_code"), 1);
    EXPECT_NE(m_err.str().find("does not sign in with"), std::string::npos);
    m_err.str("");
    m_http.enqueue(reply(400, Json{{"error", "invalid_grant"}}));
    EXPECT_EQ(run({"login", "anthropic"}), 1);
    EXPECT_NE(m_err.str().find("token exchange failed (400)"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/auth.json"));
}

TEST_F(AuthCommandTest, LogoutForgetsTheCredential) {
    m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    ASSERT_EQ(run({"login", "anthropic"}), 0);
    m_out.str("");
    EXPECT_EQ(run({"logout", "anthropic"}), 0);
    EXPECT_NE(m_out.str().find("Signed out of \"anthropic\"."), std::string::npos);
    EXPECT_FALSE(authFile().contains("anthropic"));
    m_out.str("");
    EXPECT_EQ(run({"status"}), 0);
    EXPECT_NE(m_out.str().find("No credentials are stored."), std::string::npos);
}

TEST_F(AuthCommandTest, PluginProvidersAreListedAndSignInThroughTheirPlugin) {
    EXPECT_EQ(run({"list"}), 0);
    EXPECT_EQ(m_out.str().find("hello-oauth"), std::string::npos) << "plugins load only when asked for";
    m_pluginPaths = {"plugins/hello_oauth/libhello_oauth.so"};
    m_out.str("");
    EXPECT_EQ(run({"list"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("hello-oauth\tplugin\n"), std::string::npos);

    m_in.str("abc123\n");
    m_out.str("");
    EXPECT_EQ(run({"login", "hello-oauth"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("https://hello.example/login"), std::string::npos);
    EXPECT_NE(m_out.str().find("Waiting for the code..."), std::string::npos);
    EXPECT_NE(m_out.str().find("Signed in to \"hello-oauth\"."), std::string::npos);
    const Json credential = authFile()["hello-oauth"];
    EXPECT_EQ(credential["type"], "oauth");
    EXPECT_EQ(credential["access"], "access-abc123");
    EXPECT_EQ(credential["refresh"], "refresh-abc123");
    EXPECT_EQ(credential["account"], "abc123") << "the plugin's extra fields are stored with the credential";
}

TEST_F(AuthCommandTest, APluginSignInThatFailsStoresNothing) {
    m_pluginPaths = {"plugins/hello_oauth/libhello_oauth.so"};
    m_in.str("\n");
    EXPECT_EQ(run({"login", "hello-oauth"}), 1);
    EXPECT_NE(m_err.str().find("no code entered"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/auth.json"));
}
