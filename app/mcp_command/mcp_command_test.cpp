#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.mcp_command;
import pi.testing.scripted_http_client;

class FakeCallbackServer : public ICallbackServer {
public:
    Result<int> listen(const std::string&, int, bool) override {
        return 45678;
    }
    Result<Json> waitForCallback(const std::vector<std::string>&, const std::string& state, std::chrono::milliseconds, const std::shared_ptr<AbortSignal>&) override {
        return Json{{"code", "the-code"}, {"state", state}};
    }
    void close() override {}
};

class McpCommandTest : public testing::Test {
protected:
    McpCommandTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/mcp_command_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/agent");
        std::filesystem::create_directories(m_dir + "/project");
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
        const Json config{{"mcpServers", Json{{"remote", Json{{"url", "https://mcp.example.com/mcp"}}},
                                              {"keyed", Json{{"url", "https://keyed.example.com/mcp"}, {"headers", Json{{"Authorization", "Bearer x"}}}}},
                                              {"local", Json{{"command", "/bin/true"}}},
                                              {"viaProvider", Json{{"url", "https://p.example.com/mcp"}, {"auth", Json{{"provider", "github"}}}}}}}};
        std::ofstream(m_dir + "/agent/mcp.json") << config.dump();
    }

    CommandLine line(const std::vector<std::string>& arguments) {
        CommandLine result;
        result.command = "mcp";
        result.arguments = arguments;
        result.options.cwd = m_dir + "/project";
        result.options.agentDir = m_dir + "/agent";
        return result;
    }

    int run(const std::vector<std::string>& arguments) {
        McpCommand command(*m_services, m_http, []() { return std::unique_ptr<ICallbackServer>(std::make_unique<FakeCallbackServer>()); }, m_out, m_err);
        return command.run(line(arguments));
    }

    HttpResponse reply(int status, const Json& body) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    void scriptSignIn() {
        m_http.enqueue(reply(200, Json{{"resource", "https://mcp.example.com/mcp"}, {"authorization_servers", Json::array({"https://auth.example.com"})}}));
        m_http.enqueue(reply(200, Json{{"issuer", "https://auth.example.com"}, {"authorization_endpoint", "https://auth.example.com/authorize"}, {"token_endpoint", "https://auth.example.com/token"},
                                       {"registration_endpoint", "https://auth.example.com/register"}, {"response_types_supported", Json::array({"code"})}}));
        m_http.enqueue(reply(201, Json{{"client_id", "registered"}}));
        m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}));
    }

    Json authFile() {
        return Json::parse(std::ifstream(m_dir + "/agent/mcp-auth.json"));
    }

    std::string m_dir;
    std::unique_ptr<CodingServices> m_services;
    ScriptedHttpClient m_http;
    std::ostringstream m_out;
    std::ostringstream m_err;
};

TEST_F(McpCommandTest, LoginPrintsTheUrlAndStoresTheCredentials) {
    scriptSignIn();
    EXPECT_EQ(run({"login", "remote"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("https://auth.example.com/authorize?response_type=code"), std::string::npos);
    EXPECT_NE(m_out.str().find("Signed in to \"remote\"."), std::string::npos);
    const Json file = authFile();
    ASSERT_TRUE(file.contains("mcp__remote|https://mcp.example.com/mcp"));
    EXPECT_EQ(file["mcp__remote|https://mcp.example.com/mcp"]["tokens"]["access_token"], "tok");
    EXPECT_EQ(file["mcp__remote|https://mcp.example.com/mcp"]["clientInformation"]["redirect_uris"][0], "http://127.0.0.1:45678/callback");
}

TEST_F(McpCommandTest, LoginFailuresAreReportedWithAnExitCode) {
    m_http.enqueue(reply(200, Json{{"resource", "https://mcp.example.com/mcp"}, {"authorization_servers", Json::array({"https://auth.example.com"})}}));
    m_http.enqueue(reply(200, Json{{"issuer", "https://auth.example.com"}, {"authorization_endpoint", "https://auth.example.com/authorize"}, {"token_endpoint", "https://auth.example.com/token"}, {"response_types_supported", Json::array({"code"})}}));
    EXPECT_EQ(run({"login", "remote"}), 1);
    EXPECT_NE(m_err.str().find("sign-in to \"remote\" failed"), std::string::npos);
    EXPECT_NE(m_err.str().find("oauth.clientId"), std::string::npos);
}

TEST_F(McpCommandTest, OnlyOauthServersCanSignIn) {
    EXPECT_EQ(run({"login", "local"}), 1);
    EXPECT_NE(m_err.str().find("stdio server"), std::string::npos);
    EXPECT_EQ(run({"login", "keyed"}), 1);
    EXPECT_EQ(run({"login", "viaProvider"}), 1);
    EXPECT_EQ(run({"login", "missing"}), 1);
    EXPECT_NE(m_err.str().find("no MCP server named \"missing\""), std::string::npos);
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(McpCommandTest, LogoutForgetsTheCredentials) {
    scriptSignIn();
    ASSERT_EQ(run({"login", "remote"}), 0);
    m_out.str("");
    EXPECT_EQ(run({"logout", "remote"}), 0);
    EXPECT_NE(m_out.str().find("Signed out of \"remote\"."), std::string::npos);
    EXPECT_FALSE(authFile().contains("mcp__remote|https://mcp.example.com/mcp"));
    m_out.str("");
    EXPECT_EQ(run({"logout", "remote"}), 0);
    EXPECT_NE(m_out.str().find("No credentials are stored"), std::string::npos);
}

TEST_F(McpCommandTest, ListShowsHowEachServerAuthenticates) {
    EXPECT_EQ(run({"list"}), 0);
    const std::string before = m_out.str();
    EXPECT_NE(before.find("remote\thttps://mcp.example.com/mcp\tOAuth: not signed in"), std::string::npos);
    EXPECT_NE(before.find("keyed\thttps://keyed.example.com/mcp\tno OAuth"), std::string::npos);
    EXPECT_NE(before.find("local\t/bin/true\tstdio"), std::string::npos);
    EXPECT_NE(before.find("viaProvider\thttps://p.example.com/mcp\tprovider github"), std::string::npos);
    scriptSignIn();
    ASSERT_EQ(run({"login", "remote"}), 0);
    m_out.str("");
    EXPECT_EQ(run({"list"}), 0);
    EXPECT_NE(m_out.str().find("OAuth: signed in"), std::string::npos);
}
