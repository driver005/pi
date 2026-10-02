#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.coding_application;
import pi.ai.faux_provider;
import pi.testing.capturing_byte_output;
import pi.testing.scripted_byte_input;

class CodingApplicationTest : public testing::Test {
protected:
    CodingApplicationTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/coding_application_" +
                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/project");
    }

    CodingApplicationOptions options(SessionStartMode mode) {
        CodingApplicationOptions out;
        out.cwd = m_dir + "/project";
        out.agentDir = m_dir + "/agent";
        out.faux = true;
        out.sessionMode = mode;
        return out;
    }

    std::vector<Json> lines() {
        std::vector<Json> out;
        std::istringstream stream(m_output.text());
        for (std::string line; std::getline(stream, line);) {
            out.push_back(Json::parse(line));
        }
        return out;
    }

    std::string m_dir;
    ScriptedByteInput m_input;
    CapturingByteOutput m_output;
};

TEST_F(CodingApplicationTest, RpcPromptRunsEndToEndAndPersistsTheSession) {
    CodingApplication app(options(SessionStartMode::New));
    app.services().models().faux()->enqueue(app.services().models().faux()->textResponse("hello from faux"));
    ASSERT_TRUE(app.open().has_value());
    m_input.push("{\"id\":\"s\",\"type\":\"get_state\"}\n{\"id\":\"p\",\"type\":\"prompt\",\"message\":\"hi\"}\n");
    m_input.close();
    EXPECT_EQ(app.runRpc(m_input, m_output), 0);

    bool sawState = false;
    bool sawAnswer = false;
    bool sawEnd = false;
    for (const auto& line : lines()) {
        if (line.value("id", "") == "s") {
            sawState = line["data"]["model"]["id"] == "faux-1";
        }
        if (line.value("type", "") == "message_end" && line["message"].value("role", "") == "assistant") {
            sawAnswer = line["message"].dump().find("hello from faux") != std::string::npos;
        }
        sawEnd = sawEnd || line.value("type", "") == "agent_end";
    }
    EXPECT_TRUE(sawState);
    EXPECT_TRUE(sawAnswer);
    EXPECT_TRUE(sawEnd);

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(m_dir + "/agent/sessions")) {
        files += entry.path().extension() == ".jsonl" ? 1 : 0;
    }
    EXPECT_EQ(files, 1U);
}

TEST_F(CodingApplicationTest, ContinueReopensTheLastSession) {
    std::string id;
    {
        CodingApplication first(options(SessionStartMode::New));
        first.services().models().faux()->enqueue(first.services().models().faux()->textResponse("one"));
        ASSERT_TRUE(first.open().has_value());
        id = first.runtime()->session().sessionId();
        first.runtime()->session().prompt("hi", PromptOptions{});
        first.runtime()->session().waitForIdle();
        first.runtime()->dispose();
    }
    CodingApplication second(options(SessionStartMode::Continue));
    ASSERT_TRUE(second.open().has_value());
    EXPECT_EQ(second.runtime()->session().sessionId(), id);
    EXPECT_EQ(second.runtime()->session().lastAssistantText(), "one");
}

TEST_F(CodingApplicationTest, OpenByIdAndUnknownId) {
    std::string id;
    {
        CodingApplication first(options(SessionStartMode::New));
        first.services().models().faux()->enqueue(first.services().models().faux()->textResponse("one"));
        ASSERT_TRUE(first.open().has_value());
        id = first.runtime()->session().sessionId();
        first.runtime()->session().prompt("hi", PromptOptions{});
        first.runtime()->session().waitForIdle();
        first.runtime()->dispose();
    }
    CodingApplicationOptions byId = options(SessionStartMode::Open);
    byId.sessionRef = id;
    CodingApplication found(byId);
    ASSERT_TRUE(found.open().has_value());
    EXPECT_EQ(found.runtime()->session().sessionId(), id);

    CodingApplicationOptions unknown = options(SessionStartMode::Open);
    unknown.sessionRef = "nope";
    CodingApplication missing(unknown);
    const auto opened = missing.open();
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code, "session_not_found");
}

TEST_F(CodingApplicationTest, InMemoryModeWritesNothingToDisk) {
    CodingApplication app(options(SessionStartMode::InMemory));
    ASSERT_TRUE(app.open().has_value());
    EXPECT_FALSE(app.runtime()->session().sessionFile().has_value());
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/sessions"));
}

TEST_F(CodingApplicationTest, RunWithoutOpenFails) {
    CodingApplication app(options(SessionStartMode::New));
    EXPECT_EQ(app.runRpc(m_input, m_output), 1);
}

TEST_F(CodingApplicationTest, MissingCwdFailsToOpen) {
    CodingApplicationOptions bad = options(SessionStartMode::InMemory);
    bad.cwd = m_dir + "/gone";
    CodingApplication app(bad);
    EXPECT_FALSE(app.open().has_value());
}
