#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.session_manager;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.sequential_id_generator;

class SessionManagerTest : public testing::Test {
protected:
    std::unique_ptr<SessionManager> create(bool persist = true,
                                           std::optional<std::string> file = std::nullopt) {
        SessionManagerOptions options;
        options.cwd = "/work/project";
        options.sessionDir = persist ? "/sessions" : "";
        options.persist = persist;
        options.sessionFile = file;
        auto manager = std::make_unique<SessionManager>(options, m_files, m_clock, m_ids);
        EXPECT_TRUE(manager->open().has_value());
        return manager;
    }

    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    AgentMessage assistant(const std::string& text) {
        AssistantMessage message;
        message.api = "faux";
        message.provider = "p";
        message.model = "m";
        message.content.emplace_back(TextContent{text, std::nullopt});
        message.timestamp = m_clock.nowMs();
        return message;
    }

    std::vector<Json> fileLines(const std::string& path) {
        return m_codec.parseLines(m_files.content(path));
    }

    FakeFileSystem m_files;
    FixedClock m_clock{1700000000000};
    SequentialIdGenerator m_ids{"sess"};
    SessionEntryCodec m_codec;
};

TEST_F(SessionManagerTest, NewSessionHasHeaderAndNoFileUntilConversation) {
    auto manager = create();
    EXPECT_EQ(manager->sessionId(), "sess1");
    ASSERT_TRUE(manager->sessionFile().has_value());
    EXPECT_EQ(*manager->sessionFile(), "/sessions/2023-11-14T22-13-20-000Z_sess1.jsonl");
    EXPECT_EQ(manager->header()->version, 3);
    EXPECT_EQ(manager->header()->cwd, "/work/project");
    ASSERT_TRUE(manager->appendThinkingLevelChange("high").has_value());
    ASSERT_TRUE(manager->appendModelChange("anthropic", "claude").has_value());
    EXPECT_FALSE(m_files.exists(*manager->sessionFile()));
    ASSERT_TRUE(manager->appendMessage(user("hello")).has_value());
    ASSERT_TRUE(m_files.exists(*manager->sessionFile()));
    // The earlier entries were written together with the first message.
    const auto lines = fileLines(*manager->sessionFile());
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[0]["type"], "session");
    EXPECT_EQ(lines[1]["type"], "thinking_level_change");
    EXPECT_EQ(lines[3]["message"]["content"], "hello");
}

TEST_F(SessionManagerTest, EntriesChainAndFileIsAppended) {
    auto manager = create();
    const auto first = manager->appendMessage(user("one"));
    const auto second = manager->appendMessage(assistant("two"));
    ASSERT_TRUE(first && second);
    EXPECT_EQ(manager->leafId(), *second);
    EXPECT_EQ(manager->entry(*second)->parentId, *first);
    EXPECT_FALSE(manager->entry(*first)->parentId.has_value());
    const auto lines = fileLines(*manager->sessionFile());
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[2]["parentId"], *first);
    EXPECT_EQ(manager->entryCount(), 2U);
}

TEST_F(SessionManagerTest, ReopeningRestoresTheTree) {
    std::string file;
    {
        auto manager = create();
        manager->appendMessage(user("one"));
        manager->appendMessage(assistant("two"));
        file = *manager->sessionFile();
    }
    auto reopened = create(true, file);
    EXPECT_EQ(reopened->sessionId(), "sess1");
    EXPECT_EQ(reopened->entryCount(), 2U);
    EXPECT_EQ(reopened->buildSessionContext().messages.size(), 2U);
    const auto id = reopened->appendMessage(user("three"));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(fileLines(file).size(), 4U);
}

TEST_F(SessionManagerTest, BranchingMovesLeafAndTreeShowsBothChildren) {
    auto manager = create();
    const auto first = *manager->appendMessage(user("one"));
    m_clock.advance(1000);
    const auto reply = *manager->appendMessage(assistant("two"));
    ASSERT_TRUE(manager->branch(first).has_value());
    m_clock.advance(1000);
    const auto alternative = *manager->appendMessage(assistant("alt"));
    const auto tree = manager->tree();
    ASSERT_EQ(tree.size(), 1U);
    ASSERT_EQ(tree[0].children.size(), 2U);
    EXPECT_EQ(tree[0].children[0].entry.id, reply);
    EXPECT_EQ(tree[0].children[1].entry.id, alternative);
    EXPECT_EQ(manager->children(first).size(), 2U);
    EXPECT_FALSE(manager->branch("nope").has_value());
    manager->resetLeaf();
    EXPECT_FALSE(manager->leafId().has_value());
    EXPECT_TRUE(manager->buildSessionContext().messages.empty());
}

TEST_F(SessionManagerTest, LabelsAndSessionName) {
    auto manager = create();
    const auto id = *manager->appendMessage(user("one"));
    ASSERT_TRUE(manager->appendLabelChange(id, "start").has_value());
    EXPECT_EQ(manager->label(id), "start");
    EXPECT_EQ(manager->tree()[0].label, "start");
    ASSERT_TRUE(manager->appendLabelChange(id, std::nullopt).has_value());
    EXPECT_FALSE(manager->label(id).has_value());
    EXPECT_FALSE(manager->appendLabelChange("nope", "x").has_value());
    EXPECT_FALSE(manager->sessionName().has_value());
    manager->appendSessionInfo("  My\nsession  ");
    EXPECT_EQ(manager->sessionName(), "My session");
    manager->appendSessionInfo("");
    EXPECT_FALSE(manager->sessionName().has_value());
}

TEST_F(SessionManagerTest, CompactionRecordsSystemMessageAndShortensContext) {
    auto manager = create();
    SystemMessage system;
    system.content = "be helpful";
    ASSERT_TRUE(manager->appendMessage(system).has_value());
    manager->appendMessage(user("old"));
    manager->appendMessage(assistant("old reply"));
    const auto keep = *manager->appendMessage(user("keep me"));
    manager->appendMessage(assistant("kept reply"));
    const auto compaction = manager->appendCompaction("SUMMARY", keep, 1234, Json(), std::nullopt, std::nullopt);
    ASSERT_TRUE(compaction.has_value());
    const auto entry = manager->entry(*compaction);
    EXPECT_EQ(entry->body["summary"], "SUMMARY");
    EXPECT_EQ(entry->body["firstKeptEntryId"], keep);
    EXPECT_EQ(entry->body["tokensBefore"], 1234);
    EXPECT_EQ(entry->body["systemMessage"]["content"], "be helpful");
    EXPECT_EQ(entry->body["systemMessage"]["role"], "system");
    manager->appendMessage(user("after"));
    const auto context = manager->buildSessionContext();
    // system message, compaction summary, kept user, kept reply, after
    EXPECT_EQ(context.messages.size(), 5U);
}

TEST_F(SessionManagerTest, ContextEditValidation) {
    auto manager = create();
    const auto first = *manager->appendMessage(user("secret"));
    const auto reply = *manager->appendMessage(assistant("answer"));
    ASSERT_TRUE(manager->appendContextEdit(first, Json::parse(R"({"content":"redacted"})")).has_value());
    ASSERT_TRUE(manager->appendContextEdit(reply, Json::parse(R"({"content":"short"})")).has_value());
    const auto edit = manager->entry(*manager->leafId());
    EXPECT_EQ(edit->body["replacement"]["content"][0]["text"], "short");
    EXPECT_FALSE(manager->appendContextEdit("nope", std::nullopt).has_value());
    EXPECT_FALSE(manager->appendContextEdit(first, Json::parse(R"({"content":5})")).has_value());
    const auto labelId = *manager->appendLabelChange(first, "x");
    EXPECT_FALSE(manager->appendContextEdit(labelId, std::nullopt).has_value());
}

TEST_F(SessionManagerTest, BranchWithSummaryAddsSummaryEntry) {
    auto manager = create();
    const auto first = *manager->appendMessage(user("one"));
    manager->appendMessage(assistant("two"));
    const auto summary = manager->branchWithSummary(first, "abandoned path", Json(), false, std::nullopt);
    ASSERT_TRUE(summary.has_value());
    const auto entry = manager->entry(*summary);
    EXPECT_EQ(entry->parentId, first);
    EXPECT_EQ(entry->body["summary"], "abandoned path");
    EXPECT_EQ(manager->leafId(), *summary);
    EXPECT_FALSE(manager->branchWithSummary("nope", "x", Json(), std::nullopt, std::nullopt).has_value());
}

TEST_F(SessionManagerTest, CreateBranchedSessionCopiesPathAndKeepsLabels) {
    auto manager = create();
    const auto first = *manager->appendMessage(user("one"));
    manager->appendLabelChange(first, "mark");
    const auto reply = *manager->appendMessage(assistant("two"));
    manager->branch(first);
    manager->appendMessage(assistant("other branch"));
    const std::string original = *manager->sessionFile();
    const auto branched = manager->createBranchedSession(reply);
    ASSERT_TRUE(branched.has_value());
    ASSERT_TRUE(branched->has_value());
    EXPECT_NE(**branched, original);
    EXPECT_EQ(manager->sessionId(), "sess2");
    EXPECT_EQ(manager->header()->parentSession, original);
    EXPECT_EQ(manager->buildSessionContext().messages.size(), 2U);
    EXPECT_EQ(manager->label(first), "mark");
    EXPECT_TRUE(m_files.exists(**branched));
    // Labels were removed from the path and re-added at the end.
    const auto entries = manager->entries();
    EXPECT_EQ(entries.back().type, "label");
    EXPECT_FALSE(manager->createBranchedSession("nope").has_value());
}

TEST_F(SessionManagerTest, InMemorySessionNeverWritesFiles) {
    auto manager = create(false);
    EXPECT_FALSE(manager->isPersisted());
    EXPECT_FALSE(manager->sessionFile().has_value());
    manager->appendMessage(user("hi"));
    EXPECT_EQ(manager->entryCount(), 1U);
    EXPECT_TRUE(m_files.listDirectory("/sessions").has_value() == false);
}

TEST_F(SessionManagerTest, InvalidFilesAndIds) {
    m_files.createDirectories("/sessions");
    m_files.writeFile("/sessions/garbage.jsonl", "this is not a session\n");
    SessionManagerOptions options;
    options.cwd = "/w";
    options.sessionDir = "/sessions";
    options.sessionFile = "/sessions/garbage.jsonl";
    SessionManager broken(options, m_files, m_clock, m_ids);
    const auto result = broken.open();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("not a valid pi session"), std::string::npos);
    EXPECT_EQ(m_files.content("/sessions/garbage.jsonl"), "this is not a session\n");

    auto manager = create();
    EXPECT_FALSE(manager->newSession(std::string("bad id!"), std::nullopt).has_value());
    EXPECT_FALSE(manager->newSession(std::string("-lead"), std::nullopt).has_value());
    EXPECT_TRUE(manager->newSession(std::string("good.id-1"), std::nullopt).has_value());
    EXPECT_EQ(manager->sessionId(), "good.id-1");
}

TEST_F(SessionManagerTest, OldVersionFilesAreMigratedAndRewritten) {
    m_files.createDirectories("/sessions");
    m_files.writeFile("/sessions/old.jsonl",
                      "{\"type\":\"session\",\"id\":\"old1\",\"timestamp\":\"2025-01-01T00:00:00.000Z\",\"cwd\":\"/w\"}\n"
                      "{\"type\":\"message\",\"timestamp\":\"2025-01-01T00:00:01.000Z\",\"message\":{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1}}\n"
                      "{\"type\":\"message\",\"timestamp\":\"2025-01-01T00:00:02.000Z\",\"message\":{\"role\":\"hookMessage\",\"customType\":\"x\",\"content\":\"c\",\"display\":true,\"timestamp\":2}}\n");
    auto manager = create(true, "/sessions/old.jsonl");
    EXPECT_EQ(manager->header()->version, 3);
    EXPECT_EQ(manager->entryCount(), 2U);
    const auto lines = fileLines("/sessions/old.jsonl");
    EXPECT_EQ(lines[0]["version"], 3);
    EXPECT_TRUE(lines[2].contains("parentId"));
    EXPECT_EQ(lines[2]["message"]["role"], "custom");
}

TEST_F(SessionManagerTest, MissingTrailingNewlineIsRepaired) {
    m_files.createDirectories("/sessions");
    m_files.writeFile("/sessions/a.jsonl",
                      "{\"type\":\"session\",\"version\":3,\"id\":\"a1\",\"timestamp\":\"2025-01-01T00:00:00.000Z\",\"cwd\":\"/w\"}");
    auto manager = create(true, "/sessions/a.jsonl");
    manager->appendMessage(user("x"));
    manager->appendMessage(user("y"));
    EXPECT_EQ(fileLines("/sessions/a.jsonl").size(), 3U);
}

TEST_F(SessionManagerTest, SettingSessionFileToMissingPathStartsNewSessionThere) {
    auto manager = create();
    ASSERT_TRUE(manager->setSessionFile("/sessions/custom.jsonl").has_value());
    EXPECT_EQ(manager->sessionFile(), "/sessions/custom.jsonl");
    EXPECT_EQ(manager->entryCount(), 0U);
    manager->appendMessage(user("x"));
    EXPECT_TRUE(m_files.exists("/sessions/custom.jsonl"));
}

TEST_F(SessionManagerTest, UsageAndCustomEntries) {
    auto manager = create();
    Usage usage;
    usage.input = 5;
    usage.totalTokens = 5;
    const auto entry = manager->appendUsage("cache_warm", "anthropic", "claude", usage, std::string("warm"));
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->body["kind"], "cache_warm");
    EXPECT_EQ(entry->body["usage"]["input"], 5);
    EXPECT_EQ(entry->body["note"], "warm");
    const auto custom = manager->appendCustomEntry("ext", Json::parse(R"({"a":1})"));
    EXPECT_EQ(manager->entry(*custom)->body["data"]["a"], 1);
    const auto message = manager->appendCustomMessageEntry("note", Json("text"), true, Json::parse(R"({"k":1})"));
    EXPECT_EQ(manager->entry(*message)->body["display"], true);
    // Neither usage nor plain custom entries add model context; the custom message does.
    EXPECT_EQ(manager->buildSessionContext().messages.size(), 1U);
}

TEST_F(SessionManagerTest, ResumingWithoutCwdAdoptsTheRecordedOne) {
    auto first = create();
    first->appendMessage(user("hello"));
    const std::string file = *first->sessionFile();
    SessionManagerOptions options;
    options.sessionDir = "/sessions";
    options.sessionFile = file;
    SessionManager resumed(options, m_files, m_clock, m_ids);
    ASSERT_TRUE(resumed.open().has_value());
    EXPECT_EQ(resumed.cwd(), "/work/project");
    SessionManagerOptions overridden = options;
    overridden.cwd = "/elsewhere";
    SessionManager other(overridden, m_files, m_clock, m_ids);
    ASSERT_TRUE(other.open().has_value());
    EXPECT_EQ(other.cwd(), "/elsewhere");
}
