#include <gtest/gtest.h>

import std;
import pi.support.bug_report_builder;
import pi.testing.fake_environment;
import pi.testing.fake_model_runtime;
import pi.testing.fake_system_info;
import pi.testing.fixed_clock;
import pi.testing.sequential_id_generator;

class BugReportBuilderTest : public testing::Test {
protected:
    BugReportBuilderTest() : m_environment({{"SHELL", "/bin/zsh"}, {"TERM", "xterm"}, {"PI_OFFLINE", "1"}, {"PI_API_KEY", "secret-value"}, {"HOME", "/home/me"}, {"SSH_TTY", "/dev/pts/1"}}), m_builder(m_environment, m_system, m_clock, m_ids) {}

    BugReportInput input() {
        BugReportInput out;
        out.id = "r1";
        out.hint = "  it broke  ";
        out.sessionId = "s1";
        out.cwd = "/work";
        out.messageCount = 4;
        Model model;
        model.provider = "p";
        model.id = "m";
        model.name = "M";
        model.api = "faux";
        model.baseUrl = "https://user:pw@api.example.com/v1?api_key=abc";
        model.headers = {{"x-extra", "value"}};
        model.compat = Json{{"token", "t"}, {"flag", true}};
        out.model = model;
        out.globalSettings = Json{{"trackingId", "t"}, {"deviceId", "d"}, {"theme", "dark"}, {"apiKey", "k"}};
        out.projectSettings = Json{{"defaultModel", "m"}};
        out.plugins = {"/agent/plugins/libhello.so"};
        out.pluginErrors = {{"/agent/plugins/bad.so", "boom"}};
        return out;
    }

    SessionEntry message(const std::string& id, const Json& body) {
        SessionEntry entry;
        entry.type = "message";
        entry.id = id;
        entry.timestamp = "2025-01-01T00:00:00.000Z";
        entry.body = Json{{"type", "message"}, {"id", id}, {"message", body}};
        return entry;
    }

    FakeEnvironment m_environment;
    FakeSystemInfo m_system;
    FixedClock m_clock{0};
    SequentialIdGenerator m_ids;
    BugReportBuilder m_builder;
};

TEST_F(BugReportBuilderTest, MetadataDescribesTheSetupWithoutSecrets) {
    const Json metadata = m_builder.metadata(input());
    EXPECT_EQ(metadata["schemaVersion"], 1);
    EXPECT_EQ(metadata["id"], "r1");
    EXPECT_EQ(metadata["hint"], "it broke");
    EXPECT_EQ(metadata["createdAt"], "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(metadata["environment"]["shell"], "zsh");
    EXPECT_EQ(metadata["environment"]["userAgent"], "pi/1.0.0 (linux; cpp; x64)");
    EXPECT_EQ(metadata["environment"]["terminal"]["ssh"], true);
    EXPECT_EQ(metadata["environment"]["terminal"]["tmux"], false);
    EXPECT_EQ(metadata["environment"]["piEnvironmentVariables"], Json::parse(R"(["PI_API_KEY","PI_OFFLINE"])"));
    EXPECT_FALSE(metadata["session"].contains("cwd")) << "no cwd unless the transcript is included";
    EXPECT_EQ(metadata["session"]["messageCount"], 4);
    EXPECT_EQ(metadata["model"]["baseUrl"], "https://api.example.com/v1?api_key=%3Credacted%3E");
    EXPECT_EQ(metadata["model"]["headerNames"], Json::parse(R"(["x-extra"])"));
    EXPECT_EQ(metadata["model"]["compat"]["token"], "<redacted>");
    EXPECT_EQ(metadata["settings"]["global"], Json::parse(R"({"theme":"dark","apiKey":"<redacted>"})"));
    EXPECT_EQ(metadata["extensions"][0]["origin"], "plugin");
    EXPECT_EQ(metadata["extensionErrors"][0]["error"], "boom");
    EXPECT_EQ(metadata.dump().find("secret-value"), std::string::npos);
    EXPECT_EQ(metadata.dump().find("pw@"), std::string::npos);
}

TEST_F(BugReportBuilderTest, ATranscriptAddsTheWorkingDirectoryAndAFreshIdIsGenerated) {
    BugReportInput in = input();
    in.id.reset();
    in.hint.reset();
    in.includeSession = true;
    const Json metadata = m_builder.metadata(in);
    EXPECT_EQ(metadata["session"]["cwd"], "/work");
    EXPECT_TRUE(metadata["hint"].is_null());
    EXPECT_FALSE(metadata["id"].get<std::string>().empty());
}

TEST_F(BugReportBuilderTest, DiagnosticsListFailedTurnsWithoutConversationContent) {
    const std::vector<SessionEntry> entries{
        message("u1", Json{{"role", "user"}, {"content", "my secret prompt"}}),
        message("a1", Json{{"role", "assistant"}, {"provider", "p"}, {"model", "m"}, {"api", "faux"}, {"stopReason", "stop"}, {"content", Json::array()}}),
        message("a2", Json{{"role", "assistant"}, {"provider", "p"}, {"model", "m"}, {"api", "faux"}, {"stopReason", "error"}, {"errorMessage", "429"}, {"rawStopReason", "x"}, {"content", Json::array({Json{{"type", "text"}, {"text", "private"}}})}}),
        message("a3", Json{{"role", "assistant"}, {"provider", "p"}, {"model", "m"}, {"api", "faux"}, {"stopReason", "stop"}, {"diagnostics", Json::array({Json{{"kind", "retry"}}})}}),
    };
    CrashRecord crash;
    crash.timestamp = "2025-01-01T00:00:00.000Z";
    crash.kind = "fatal_error";
    crash.message = "segv";
    crash.notified = true;
    const Json diagnostics = m_builder.diagnostics("s1", entries, {crash});
    EXPECT_EQ(diagnostics["entryCount"], 4);
    EXPECT_EQ(diagnostics["assistantMessageCount"], 3);
    ASSERT_EQ(diagnostics["assistant"].size(), 2U);
    EXPECT_EQ(diagnostics["assistant"][0]["entryId"], "a2");
    EXPECT_EQ(diagnostics["assistant"][0]["errorMessage"], "429");
    EXPECT_EQ(diagnostics["assistant"][0]["rawStopReason"], "x");
    EXPECT_EQ(diagnostics["assistant"][1]["diagnostics"][0]["kind"], "retry");
    EXPECT_EQ(diagnostics["crashes"][0]["message"], "segv");
    EXPECT_FALSE(diagnostics["crashes"][0].contains("notified"));
    EXPECT_EQ(diagnostics.dump().find("private"), std::string::npos);
    EXPECT_EQ(diagnostics.dump().find("secret prompt"), std::string::npos);
}

TEST_F(BugReportBuilderTest, FilesFollowTheBundle) {
    BugReportBundle bundle;
    bundle.metadata = Json{{"a", 1}};
    bundle.diagnostics = Json{{"b", 2}};
    auto files = m_builder.files(bundle);
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].name, "report.json");
    EXPECT_EQ(files[0].data, "{\n  \"a\": 1\n}\n");
    bundle.sessionJsonl = "{}\n";
    bundle.summary = "# S";
    files = m_builder.files(bundle);
    ASSERT_EQ(files.size(), 4U);
    EXPECT_EQ(files[2].contentType, "application/x-ndjson");
    EXPECT_EQ(files[3].name, "summary.md");
    EXPECT_EQ(files[3].data, "# S\n");
    EXPECT_EQ(m_builder.archiveFileName("r1"), "pi-bug-report-r1.zip");
    EXPECT_EQ(m_builder.customEntryType(), "pi.bug-report");
}

TEST_F(BugReportBuilderTest, ProvidersAreDescribedByAuthStatusWithoutKeys) {
    FakeModelRuntime models;
    Model model;
    model.provider = "p";
    model.id = "m";
    models.addModel(model);
    models.setAuthenticated("p", true);
    const Json provider = m_builder.describeProvider(models, "p", "https://u:p@h.io/x");
    EXPECT_EQ(provider["id"], "p");
    EXPECT_EQ(provider["baseUrl"], "https://h.io/x");
    EXPECT_EQ(provider["authStatus"]["configured"], true);
    EXPECT_TRUE(m_builder.describeProvider(models, "nope", std::nullopt).is_null());
}
