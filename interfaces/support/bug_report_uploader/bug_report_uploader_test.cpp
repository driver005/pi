#include <gtest/gtest.h>

import std;
import pi.support.bug_report_uploader;
import pi.testing.scripted_http_client;
import pi.testing.sequential_id_generator;

class BugReportUploaderTest : public testing::Test {
protected:
    HttpResponse reply(int status, const std::string& body) {
        HttpResponse response;
        response.status = status;
        response.body = body;
        return response;
    }

    std::vector<BugReportFile> files() {
        return {{"report.json", "application/json", "{}\n"}, {"summary.md", "text/markdown", "# S\n"}};
    }

    ScriptedHttpClient m_http;
    SequentialIdGenerator m_ids;
    BugReportUploader m_uploader{m_http, m_ids};
};

TEST_F(BugReportUploaderTest, PostsMultipartFormDataAndReturnsTheReportId) {
    m_http.enqueue(reply(200, R"({"ok":true,"bug_report":{"id":"br_1"}})"));
    const auto id = m_uploader.upload(files(), "https://gw.example", "tok");
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_EQ(*id, "br_1");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.url, "https://gw.example/v1/bug-reports");
    std::map<std::string, std::string> headers(request.headers.begin(), request.headers.end());
    EXPECT_EQ(headers["Authorization"], "Bearer tok");
    const std::string type = headers["Content-Type"];
    ASSERT_TRUE(type.starts_with("multipart/form-data; boundary="));
    const std::string boundary = type.substr(std::string("multipart/form-data; boundary=").size());
    EXPECT_NE(request.body.find("--" + boundary + "\r\nContent-Disposition: form-data; name=\"report.json\"; filename=\"report.json\"\r\nContent-Type: application/json\r\n\r\n{}\n\r\n"), std::string::npos);
    EXPECT_NE(request.body.find("name=\"summary.md\""), std::string::npos);
    EXPECT_TRUE(request.body.ends_with("--" + boundary + "--\r\n"));
}

TEST_F(BugReportUploaderTest, AnonymousUploadsSendNoAuthorization) {
    m_http.enqueue(reply(200, R"({"ok":true,"bug_report":{"id":"x"}})"));
    ASSERT_TRUE(m_uploader.upload(files(), "https://gw.example", std::nullopt).has_value());
    const HttpRequest sent = m_http.requests()[0];
    for (const auto& [name, value] : sent.headers) {
        EXPECT_NE(name, "Authorization");
    }
}

TEST_F(BugReportUploaderTest, FailuresCarryTheGatewaysDescription) {
    m_http.enqueue(reply(413, R"({"ok":false,"error":"too_large","description":"Report is too large"})"));
    auto failed = m_uploader.upload(files(), "https://gw.example", std::nullopt);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Bug report upload failed: Report is too large");
    m_http.enqueue(reply(500, "oops"));
    failed = m_uploader.upload(files(), "https://gw.example", std::nullopt);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Bug report upload failed: 500");
    m_http.enqueue(std::unexpected(Error{"timeout", "timed out"}));
    failed = m_uploader.upload(files(), "https://gw.example", std::nullopt);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "timeout");
}
