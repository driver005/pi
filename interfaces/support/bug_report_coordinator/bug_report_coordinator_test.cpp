#include <gtest/gtest.h>

import std;
import pi.support.bug_report_coordinator;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fake_system_info;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;
import pi.testing.sequential_id_generator;
import pi.testing.session_harness;

class BugReportCoordinatorTest : public testing::Test {
protected:
    BugReportCoordinatorTest() : m_harness("/work") {
        m_files.createDirectories("/agent");
        m_files.createDirectories("/work");
        m_options.model.id = "m";
        m_options.model.provider = "p";
        m_options.model.api = "faux";
        m_options.model.contextWindow = 100000;
        m_options.streamFn = [](const Model& model, const TranscriptContext&, const StreamOptions&) {
            auto stream = std::make_shared<AssistantMessageStream>([](const AssistantMessageEvent& event) { return event.type == AssistantEventType::Done; }, [](const AssistantMessageEvent& event) { return *event.message; });
            AssistantMessage message;
            message.provider = model.provider;
            message.stopReason = StopReason::Stop;
            TextContent text;
            text.text = "SUMMARY";
            message.content.push_back(text);
            AssistantMessageEvent event;
            event.type = AssistantEventType::Done;
            event.reason = StopReason::Stop;
            event.message = std::make_shared<const AssistantMessage>(message);
            stream->push(event);
            return stream;
        };
        UserMessage user;
        user.content = std::string("hello");
        user.timestamp = 1;
        m_harness.session().appendMessage(user);
        AssistantMessage failed;
        failed.stopReason = StopReason::Error;
        failed.errorMessage = "429";
        failed.provider = "p";
        failed.model = "m";
        failed.timestamp = 2;
        m_harness.session().appendMessage(failed);
    }

    BugReportInput input() {
        BugReportInput out;
        out.sessionId = m_harness.session().sessionId();
        out.cwd = "/work";
        out.messageCount = 2;
        return out;
    }

    Result<BugReportOutcome> report(const BugReportRequest& request, const std::optional<std::string>& token = std::nullopt) {
        return m_coordinator.report(request, input(), {}, m_options, m_harness.session(), [token] { return token; }, "/agent", "/work", "https://gw.example");
    }

    std::vector<SessionEntry> bugEntries() {
        std::vector<SessionEntry> out;
        for (const SessionEntry& entry : m_harness.session().entries()) {
            if (entry.type == "custom" && entry.body.value("customType", std::string()) == "pi.bug-report") {
                out.push_back(entry);
            }
        }
        return out;
    }

    SessionHarness m_harness;
    FakeFileSystem& m_files = m_harness.files();
    FakeEnvironment m_environment;
    FakeSystemInfo m_system;
    SequentialIdGenerator m_ids;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    BugReportBuilder m_builder{m_environment, m_system, m_harness.clock(), m_ids};
    SummaryGenerator m_generator{m_harness.clock(), m_ids, m_sleeper};
    BugReportSummarizer m_summarizer{m_generator};
    SessionBranchSerializer m_serializer{m_harness.clock()};
    BugReportUploader m_uploader{m_http, m_ids};
    CrashLog m_crashLog{m_files, m_harness.clock()};
    BugReportCoordinator m_coordinator{m_builder, m_summarizer, m_serializer, m_uploader, m_crashLog, m_files, m_harness.clock(), m_environment};
    SummarizationOptions m_options;
};

TEST_F(BugReportCoordinatorTest, ZipDeliveryWritesTheArchiveAndRecordsItInTheSession) {
    BugReportRequest request;
    request.hint = "it broke";
    request.includeSession = true;
    const auto outcome = report(request);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(outcome->delivery, "zip");
    ASSERT_TRUE(outcome->path.has_value());
    EXPECT_TRUE(outcome->path->starts_with("/work/pi-bug-report-"));
    EXPECT_TRUE(outcome->path->ends_with(".zip"));
    const auto archive = m_files.readFile(*outcome->path);
    ASSERT_TRUE(archive.has_value());
    EXPECT_EQ(archive->substr(0, 2), "PK");
    EXPECT_NE(archive->find("session.jsonl"), std::string::npos);
    EXPECT_NE(archive->find("\"it broke\""), std::string::npos);
    ASSERT_EQ(bugEntries().size(), 1U);
    EXPECT_EQ(bugEntries()[0].body["data"]["delivery"], "zip");
    EXPECT_EQ(bugEntries()[0].body["data"]["path"], *outcome->path);
    EXPECT_EQ(bugEntries()[0].body["data"]["sessionIncluded"], true);
}

TEST_F(BugReportCoordinatorTest, TheOutputPathIsAFileOrADirectory) {
    BugReportRequest request;
    request.outputPath = "/work/custom.zip";
    EXPECT_EQ(*report(request)->path, "/work/custom.zip");
    m_files.createDirectories("/out");
    request.outputPath = "/out";
    EXPECT_TRUE(report(request)->path->starts_with("/out/pi-bug-report-"));
    request.outputPath = "/nowhere/dir.zip";
    const auto failed = report(request);
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().message.find("Failed to write bug report"), std::string::npos);
}

TEST_F(BugReportCoordinatorTest, SummariesComeFromTheModelAndTheTranscriptStaysOut) {
    BugReportRequest request;
    request.includeSummary = true;
    const auto outcome = report(request);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    const auto archive = m_files.readFile(*outcome->path);
    EXPECT_NE(archive->find("summary.md"), std::string::npos);
    EXPECT_NE(archive->find("SUMMARY"), std::string::npos);
    EXPECT_EQ(archive->find("session.jsonl"), std::string::npos);
    EXPECT_EQ(bugEntries()[0].body["data"]["summaryIncluded"], true);
}

TEST_F(BugReportCoordinatorTest, UploadsUseTheRadiusTokenAndReturnTheGatewaysId) {
    HttpResponse response;
    response.status = 200;
    response.body = R"({"ok":true,"bug_report":{"id":"br_9"}})";
    m_http.enqueue(response);
    BugReportRequest request;
    request.delivery = "upload";
    const auto outcome = report(request, "radius-token");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(outcome->id, "br_9");
    EXPECT_FALSE(outcome->path.has_value());
    const HttpRequest sent = m_http.requests()[0];
    EXPECT_EQ(sent.url, "https://gw.example/v1/bug-reports");
    bool authorized = false;
    for (const auto& [name, value] : sent.headers) {
        authorized = authorized || (name == "Authorization" && value == "Bearer radius-token");
    }
    EXPECT_TRUE(authorized);
    EXPECT_EQ(bugEntries()[0].body["data"]["delivery"], "upload");
}

TEST_F(BugReportCoordinatorTest, OfflineModeRefusesUploadsAndBadDeliveriesAreRejected) {
    m_environment.set("PI_OFFLINE", "1");
    BugReportRequest request;
    request.delivery = "upload";
    const auto offline = report(request);
    ASSERT_FALSE(offline.has_value());
    EXPECT_EQ(offline.error().code, "offline");
    EXPECT_EQ(m_http.calls(), 0);
    request.delivery = "email";
    EXPECT_EQ(report(request).error().code, "invalid_argument");
    EXPECT_TRUE(bugEntries().empty());
}

TEST_F(BugReportCoordinatorTest, ReportedCrashesAreClearedAfterDelivery) {
    m_crashLog.record("/agent/crashes.json", "fatal_error", "segfault", std::nullopt, std::nullopt, "/work");
    BugReportRequest request;
    const auto outcome = report(request);
    ASSERT_TRUE(outcome.has_value());
    const auto archive = m_files.readFile(*outcome->path);
    EXPECT_NE(archive->find("segfault"), std::string::npos);
    EXPECT_FALSE(m_files.exists("/agent/crashes.json"));
}

TEST_F(BugReportCoordinatorTest, AFailedSummaryStopsTheReport) {
    m_options.streamFn = nullptr;
    BugReportRequest request;
    request.includeSummary = true;
    const auto failed = report(request);
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().message.find("Failed to write bug report summary"), std::string::npos);
    EXPECT_TRUE(bugEntries().empty());
}
