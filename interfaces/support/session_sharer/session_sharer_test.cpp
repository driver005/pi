#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

import std;
import pi.support.header_merger;
import pi.support.session_sharer;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.scripted_http_client;
import pi.testing.scripted_process_runner;
import pi.testing.sequential_id_generator;

class SessionSharerTest : public testing::Test {
protected:
    ProcessResult exited(int code, std::string output = "") {
        ProcessResult result;
        result.exitCode = code;
        result.output = std::move(output);
        return result;
    }

    std::function<Result<std::string>()> html() {
        return [this]() -> Result<std::string> {
            ++m_rendered;
            return std::string("<html>session</html>");
        };
    }

    HttpResponse reply(int status, std::string body) {
        HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        return response;
    }

    FakeEnvironment m_environment;
    FakeFileSystem m_files;
    ScriptedHttpClient m_http;
    SequentialIdGenerator m_ids;
    std::string m_gistFile;
    std::string m_gistContent;
    std::function<Result<ProcessResult>(const ProcessRequest&)> m_gh =
        [this](const ProcessRequest& request) -> Result<ProcessResult> {
        if (request.args[0] == "auth") {
            return exited(0, "Logged in");
        }
        m_gistFile = request.args.back();
        m_gistContent = m_files.content(m_gistFile);
        return exited(0, "https://gist.github.com/someone/abc123def\n");
    };
    ScriptedProcessRunner m_processes{
        [this](const ProcessRequest& request) { return m_gh(request); }};
    SessionSharer m_sharer{m_http, m_processes, m_files, m_environment, m_ids};
    int m_rendered = 0;
};

TEST_F(SessionSharerTest, UploadsToRadiusWhenSignedIn) {
    m_http.enqueue(reply(200, R"({"artifact":{"canonical_url":"https://radius.pi.dev/a/1"}})"));
    const auto outcome = m_sharer.share("{\"type\":\"session\"}\n", html(), "token", nullptr);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(outcome->route, ShareRoute::Radius);
    EXPECT_EQ(outcome->url, "https://radius.pi.dev/a/1");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.url,
              "https://radius.pi.dev/v1/artifacts?visibility=organization&title=Pi+session");
    EXPECT_EQ(request.body, "{\"type\":\"session\"}\n");
    HeaderMerger headers;
    EXPECT_EQ(headers.find(request.headers, "authorization").value_or(""), "Bearer token");
    EXPECT_EQ(headers.find(request.headers, "content-type").value_or(""), "application/x-ndjson");
    EXPECT_EQ(m_rendered, 0) << "the page is only rendered for a gist";
    EXPECT_EQ(m_processes.calls(), 0);
}

TEST_F(SessionSharerTest, UploadsToTheConfiguredGateway) {
    m_environment.set("PI_RADIUS_GATEWAY", "radius.corp.example/");
    m_http.enqueue(
        reply(200, R"({"artifact":{"canonical_url":"https://radius.corp.example/a/2"}})"));
    ASSERT_TRUE(m_sharer.share("x", html(), "token", nullptr).has_value());
    EXPECT_EQ(m_http.requests()[0].url,
              "https://radius.corp.example/v1/artifacts?visibility=organization&title=Pi+session");
}

TEST_F(SessionSharerTest, ARejectedRadiusUploadDoesNotFallBackToAGist) {
    m_http.enqueue(reply(403, R"({"error":"not allowed"})"));
    const auto denied = m_sharer.share("x", html(), "token", nullptr);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().message, "Failed to upload Radius artifact: not allowed");
    m_http.enqueue(reply(502, "bad gateway"));
    EXPECT_EQ(m_sharer.share("x", html(), "token", nullptr).error().message,
              "Failed to upload Radius artifact: 502");
    m_http.enqueue(reply(200, R"({"artifact":{}})"));
    EXPECT_EQ(m_sharer.share("x", html(), "token", nullptr).error().message,
              "Failed to upload Radius artifact: 200");
    EXPECT_EQ(m_processes.calls(), 0);
}

TEST_F(SessionSharerTest, SharesAPrivateGistThroughTheGitHubCliWithoutRadius) {
    const auto outcome = m_sharer.share("x", html(), std::nullopt, nullptr);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(outcome->route, ShareRoute::Gist);
    EXPECT_EQ(outcome->url, "https://pi.dev/session/#abc123def");
    EXPECT_EQ(outcome->gistUrl, "https://gist.github.com/someone/abc123def");
    const auto requests = m_processes.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].command, "gh");
    EXPECT_EQ(requests[0].args, (std::vector<std::string>{"auth", "status"}));
    EXPECT_EQ(requests[1].args[0], "gist");
    EXPECT_EQ(requests[1].args[2], "--public=false");
    EXPECT_TRUE(m_gistFile.ends_with("/session.html"));
    EXPECT_EQ(m_gistContent, "<html>session</html>");
    EXPECT_FALSE(m_files.exists(m_gistFile)) << "the temporary page is removed";
}

TEST_F(SessionSharerTest, TheViewerUrlCanBeOverridden) {
    m_environment.set("PI_SHARE_VIEWER_URL", "https://viewer.test/s");
    EXPECT_EQ(m_sharer.share("x", html(), std::nullopt, nullptr)->url,
              "https://viewer.test/s#abc123def");
}

TEST_F(SessionSharerTest, ExplainsWhyAGistCannotBeCreated) {
    m_gh = [](const ProcessRequest&) -> Result<ProcessResult> {
        return std::unexpected(Error{"spawn", "no such file"});
    };
    EXPECT_EQ(m_sharer.share("x", html(), std::nullopt, nullptr).error().message,
              "GitHub CLI (gh) is not installed. Install it from https://cli.github.com/");
    m_gh = [this](const ProcessRequest&) -> Result<ProcessResult> {
        return exited(1, "not logged in");
    };
    EXPECT_EQ(m_sharer.share("x", html(), std::nullopt, nullptr).error().message,
              "GitHub CLI is not logged in. Run 'gh auth login' first.");
    m_gh = [this](const ProcessRequest& request) -> Result<ProcessResult> {
        return request.args[0] == "auth" ? exited(0) : exited(1, "quota exceeded\n");
    };
    EXPECT_EQ(m_sharer.share("x", html(), std::nullopt, nullptr).error().message,
              "Failed to create gist: quota exceeded");
    m_gh = [this](const ProcessRequest& request) -> Result<ProcessResult> {
        return request.args[0] == "auth" ? exited(0) : exited(0, "\n");
    };
    EXPECT_EQ(m_sharer.share("x", html(), std::nullopt, nullptr).error().message,
              "Failed to parse gist ID from gh output");
    EXPECT_EQ(
        m_sharer
            .share(
                "x",
                []() -> Result<std::string> { return std::unexpected(Error{"x", "no export"}); },
                std::nullopt, nullptr)
            .error()
            .message.substr(0, 24),
        "Failed to export session");
}
