#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.child_process_launcher;
import pi.mcp_connector;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.header_merger;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;
import pi.testing.scripted_process_runner;


class PassthroughLock : public IFileLock {
public:
    Result<void> withLock(const std::string&, const std::function<Result<void>()>& action) override {
        return action();
    }
};

class McpConnectorTest : public testing::Test {
protected:
    McpConnectorTest()
        : m_environment({{"SECRET", "s3cret"}, {"TOKEN", "t0k"}}),
          m_runner([](const ProcessRequest&) -> Result<ProcessResult> { return ProcessResult{}; }),
          m_resolver(m_environment, m_runner),
          m_connector(m_launcher, m_http, m_sleeper, m_files, m_resolver,
                      [](const std::string& provider) -> std::optional<std::string> {
                          return provider == "known" ? std::optional<std::string>("provider-token") : std::nullopt;
                      }) {
        m_options.requestTimeoutMs = 5000;
    }

    McpServerConfig stdio(const std::string& script) {
        McpServerConfig config;
        config.name = "local";
        config.command = "/bin/sh";
        config.args = {"-c", script};
        return config;
    }

    McpServerConfig http() {
        McpServerConfig config;
        config.name = "remote";
        config.http = true;
        config.url = "http://mcp.test/mcp";
        return config;
    }

    HttpResponse reply(int status, const std::string& type, const std::string& body, HttpHeaders headers = {}) {
        HttpResponse response;
        response.status = status;
        response.body = body;
        if (!type.empty()) {
            response.headers.emplace_back("Content-Type", type);
        }
        for (auto& header : headers) {
            response.headers.push_back(std::move(header));
        }
        return response;
    }

    Json initializeResult() {
        return Json{{"jsonrpc", "2.0"}, {"id", 1},
                    {"result", Json{{"protocolVersion", "2025-11-25"}, {"capabilities", Json::object()},
                                    {"serverInfo", Json{{"name", "h"}, {"version", "1"}}}, {"instructions", "remote hi"}}}};
    }

    std::string header(const HttpRequest& request, const std::string& name) {
        return HeaderMerger().find(request.headers, name).value_or("");
    }

    // A shell MCP server: answers initialize with $REPLY_TEXT as the instructions, ignores the rest.
    const std::string m_server = R"sh(
while IFS= read -r line; do
  case "$line" in
    *'"method":"initialize"'*)
      id=${line#*\"id\":}; id=${id%%,*}
      printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":{"name":"sh","version":"1"},"instructions":"%s"}}\n' "$id" "$REPLY_TEXT"
      ;;
  esac
done
)sh";

    FakeFileSystem m_files;
    FakeEnvironment m_environment;
    ScriptedProcessRunner m_runner;
    ConfigValueResolver m_resolver;
    ChildProcessLauncher m_launcher;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    McpClientOptions m_options;
    McpConnector m_connector;
};

TEST_F(McpConnectorTest, ConnectsToStdioServersWithResolvedEnvironment) {
    McpServerConfig config = stdio(m_server);
    config.env["REPLY_TEXT"] = "${SECRET}";
    const auto client = m_connector.connect(config, "/", m_options);
    ASSERT_TRUE(client.has_value());
    EXPECT_TRUE((*client)->connected());
    EXPECT_EQ(*(*client)->instructions(), "s3cret");
}

TEST_F(McpConnectorTest, StdioServersRunInTheResolvedDirectory) {
    McpServerConfig config = stdio(std::string("export REPLY_TEXT=\"$PWD\"\n") + m_server);
    config.cwd = "usr";
    const auto client = m_connector.connect(config, "/", m_options);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ(*(*client)->instructions(), "/usr");
}

TEST_F(McpConnectorTest, StdioFailuresCarryTheStderrTail) {
    const auto client = m_connector.connect(stdio("echo 'cannot start: oops' >&2; exit 3"), "/", m_options);
    ASSERT_FALSE(client.has_value());
    EXPECT_NE(client.error().message.find("cannot start: oops"), std::string::npos);
}

TEST_F(McpConnectorTest, UnresolvableValuesNameTheServer) {
    McpServerConfig config = stdio(m_server);
    config.env["KEY"] = "${NOPE_NOT_SET}";
    const auto client = m_connector.connect(config, "/", m_options);
    ASSERT_FALSE(client.has_value());
    EXPECT_NE(client.error().message.find("MCP server \"local\" env \"KEY\""), std::string::npos);
}

TEST_F(McpConnectorTest, ConnectsToHttpServersWithResolvedHeaders) {
    McpServerConfig config = http();
    config.headers["Authorization"] = "Bearer ${TOKEN}";
    m_http.enqueue(reply(200, "application/json", initializeResult().dump(), {{"Mcp-Session-Id", "s1"}}));
    m_http.enqueue(reply(202, "", ""));
    m_http.enqueue(reply(200, "", ""));
    {
        const auto client = m_connector.connect(config, "/", m_options);
        ASSERT_TRUE(client.has_value());
        EXPECT_EQ(*(*client)->instructions(), "remote hi");
    }
    const auto sent = m_http.requests();
    ASSERT_GE(sent.size(), 2U);
    EXPECT_EQ(header(sent[0], "authorization"), "Bearer t0k");
    EXPECT_EQ(sent[0].url, "http://mcp.test/mcp");
}

TEST_F(McpConnectorTest, ProviderTokensAreSentAsBearerTokens) {
    McpServerConfig config = http();
    config.authProvider = "known";
    m_http.enqueue(reply(200, "application/json", initializeResult().dump()));
    m_http.enqueue(reply(202, "", ""));
    const auto client = m_connector.connect(config, "/", m_options);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ(header(m_http.requests()[0], "authorization"), "Bearer provider-token");
}

TEST_F(McpConnectorTest, HttpAuthenticationFailuresKeepTheirCode) {
    m_http.enqueue(reply(401, "text/plain", "no"));
    const auto client = m_connector.connect(http(), "/", m_options);
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(client.error().code, "auth_required");
}

TEST_F(McpConnectorTest, OauthServersSendTheStoredTokenAndRefreshAfterA401) {
    PassthroughLock lock;
    BoringCrypto crypto;
    Base64Codec base64;
    FixedClock clock(1'000'000);
    m_files.createDirectories("/agent");
    McpOauthStore store("/agent/mcp-auth.json", "/agent", m_files, lock, crypto);
    McpOauthRefresher refresher(m_http, clock, base64);
    McpOauthProviders providers(store, refresher, clock, m_resolver);
    McpConnector connector(m_launcher, m_http, m_sleeper, m_files, m_resolver, [](const std::string&) { return std::optional<std::string>(); }, &providers);
    const McpServerConfig config = http();
    const Json state{{"serverUrl", "http://mcp.test/mcp"},
                     {"clientInformation", Json{{"client_id", "c1"}}},
                     {"tokens", Json{{"access_token", "stored"}, {"token_type", "Bearer"}, {"refresh_token", "r1"}}},
                     {"discovery", Json{{"authorizationServerUrl", "http://localhost:9"},
                                        {"authorizationServerMetadata", Json{{"issuer", "http://localhost:9"}, {"authorization_endpoint", "http://localhost:9/a"}, {"token_endpoint", "http://localhost:9/token"}, {"response_types_supported", Json::array({"code"})}}}}}};
    ASSERT_TRUE(store.save("remote", config.url, state).has_value());
    // The server rejects the stored token, the refresh yields a new one, the retried request is accepted.
    m_http.enqueue(reply(401, "text/plain", "expired", {{"WWW-Authenticate", "Bearer error=\"invalid_token\""}}));
    m_http.enqueue(reply(200, "application/json", Json{{"access_token", "fresh"}, {"token_type", "Bearer"}}.dump()));
    m_http.enqueue(reply(200, "application/json", initializeResult().dump()));
    m_http.enqueue(reply(202, "", ""));
    const auto client = connector.connect(config, "/", m_options);
    ASSERT_TRUE(client.has_value()) << client.error().message;
    const std::vector<HttpRequest> requests = m_http.requests();
    EXPECT_EQ(header(requests[0], "authorization"), "Bearer stored");
    EXPECT_EQ(requests[1].url, "http://localhost:9/token");
    EXPECT_EQ(header(requests[2], "authorization"), "Bearer fresh");
    EXPECT_EQ((**store.load("remote", config.url))["tokens"]["access_token"], "fresh");
}

TEST_F(McpConnectorTest, ServersWithAnAuthorizationHeaderDoNotUseOauth) {
    PassthroughLock lock;
    BoringCrypto crypto;
    Base64Codec base64;
    FixedClock clock(1'000'000);
    m_files.createDirectories("/agent");
    McpOauthStore store("/agent/mcp-auth.json", "/agent", m_files, lock, crypto);
    McpOauthRefresher refresher(m_http, clock, base64);
    McpOauthProviders providers(store, refresher, clock, m_resolver);
    McpConnector connector(m_launcher, m_http, m_sleeper, m_files, m_resolver, [](const std::string&) { return std::optional<std::string>(); }, &providers);
    McpServerConfig config = http();
    config.headers["Authorization"] = "Bearer static";
    m_http.enqueue(reply(401, "text/plain", "no"));
    const auto client = connector.connect(config, "/", m_options);
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(m_http.calls(), 1);
    EXPECT_EQ(header(m_http.requests()[0], "authorization"), "Bearer static");
}
