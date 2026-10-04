#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.session_manager;
import pi.session.session_store;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.sequential_id_generator;

class TestSessionFactory : public ISessionManagerFactory {
public:
    TestSessionFactory(IFileSystem& files, const IClock& clock, IIdGenerator& ids)
        : m_files(files), m_clock(clock), m_ids(ids) {}

    Result<std::unique_ptr<ISessionManager>> create(const SessionManagerOptions& options) override {
        auto manager = std::make_unique<SessionManager>(options, m_files, m_clock, m_ids);
        if (auto opened = manager->open(); !opened) {
            return std::unexpected(opened.error());
        }
        return std::unique_ptr<ISessionManager>(std::move(manager));
    }

private:
    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
};

class SessionStoreTest : public testing::Test {
protected:
    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    FakeFileSystem m_files;
    FixedClock m_clock{1700000000000};
    SequentialIdGenerator m_ids{"s"};
    TestSessionFactory m_factory{m_files, m_clock, m_ids};
    SessionStore m_impl{"/home/user/.pi/agent", m_files, m_clock, m_ids, m_factory};
    ISessionStore& m_store = m_impl;
};

TEST_F(SessionStoreTest, DefaultSessionDirEncodesCwd) {
    const std::string dir = m_store.defaultSessionDir("/work/my:project");
    EXPECT_EQ(dir, "/home/user/.pi/agent/sessions/--work-my-project--");
    EXPECT_TRUE(m_files.exists(dir));
}

TEST_F(SessionStoreTest, CreatePersistsIntoDefaultDir) {
    auto manager = m_store.create("/work/a");
    ASSERT_TRUE(manager.has_value());
    ASSERT_TRUE((*manager)->appendMessage(user("hello")).has_value());
    ASSERT_TRUE((*manager)->sessionFile().has_value());
    EXPECT_EQ((*manager)->sessionFile()->rfind("/home/user/.pi/agent/sessions/--work-a--/", 0), 0U);
    EXPECT_TRUE(m_files.exists(*(*manager)->sessionFile()));
}

TEST_F(SessionStoreTest, OpenUsesHeaderCwdAndParentDir) {
    auto created = m_store.create("/work/a");
    (*created)->appendMessage(user("hello"));
    const std::string file = *(*created)->sessionFile();
    auto reopened = m_store.open(file);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ((*reopened)->cwd(), "/work/a");
    EXPECT_EQ((*reopened)->entryCount(), 1U);
    auto overridden = m_store.open(file, std::nullopt, std::string("/elsewhere"));
    EXPECT_EQ((*overridden)->cwd(), "/elsewhere");
}

TEST_F(SessionStoreTest, ContinueRecentPicksNewestOtherwiseStartsFresh) {
    auto fresh = m_store.continueRecent("/work/a");
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ((*fresh)->entryCount(), 0U);
    auto first = m_store.create("/work/a");
    (*first)->appendMessage(user("older"));
    m_files.setNowMs(1700000050000);
    m_clock.advance(60000);
    auto second = m_store.create("/work/a");
    (*second)->appendMessage(user("newer"));
    const std::string newer = *(*second)->sessionFile();
    auto resumed = m_store.continueRecent("/work/a");
    ASSERT_TRUE(resumed.has_value());
    EXPECT_EQ(*(*resumed)->sessionFile(), newer);
}

TEST_F(SessionStoreTest, FindByIdAndMostRecentWithCwdFilter) {
    auto a = m_store.create("/work/a", std::string("/shared"), std::string("alpha"));
    (*a)->appendMessage(user("x"));
    auto b = m_store.create("/work/b", std::string("/shared"), std::string("beta"));
    (*b)->appendMessage(user("y"));
    EXPECT_TRUE(m_store.findById("/work/a", "alpha", std::string("/shared")).has_value());
    EXPECT_FALSE(m_store.findById("/work/a", "beta", std::string("/shared")).has_value());
    EXPECT_FALSE(m_store.findById("/work/a", "nope", std::string("/shared")).has_value());
    const auto recent = m_store.findMostRecent("/shared", std::string("/work/b"));
    ASSERT_TRUE(recent.has_value());
    EXPECT_NE(recent->find("beta"), std::string::npos);
    EXPECT_FALSE(m_store.findMostRecent("/shared", std::string("/work/none")).has_value());
}

TEST_F(SessionStoreTest, ListFiltersCustomDirByCwdAndSortsByActivity) {
    auto a = m_store.create("/work/a", std::string("/shared"), std::string("alpha"));
    (*a)->appendMessage(user("first session"));
    m_clock.advance(120000);
    auto b = m_store.create("/work/a", std::string("/shared"), std::string("gamma"));
    (*b)->appendMessage(user("second session"));
    auto other = m_store.create("/work/z", std::string("/shared"), std::string("zed"));
    (*other)->appendMessage(user("other project"));
    const auto sessions = m_store.list("/work/a", std::string("/shared"));
    ASSERT_EQ(sessions.size(), 2U);
    EXPECT_EQ(sessions[0].id, "gamma");
    EXPECT_EQ(sessions[0].firstMessage, "second session");
    EXPECT_EQ(sessions[1].id, "alpha");
    EXPECT_EQ(m_store.listAll(std::string("/shared")).size(), 3U);
}

TEST_F(SessionStoreTest, ListAllWalksProjectDirectories) {
    auto a = m_store.create("/work/a");
    (*a)->appendMessage(user("in a"));
    auto b = m_store.create("/work/b");
    (*b)->appendMessage(user("in b"));
    const auto all = m_store.listAll();
    EXPECT_EQ(all.size(), 2U);
    EXPECT_EQ(m_store.list("/work/a").size(), 1U);
}

TEST_F(SessionStoreTest, ForkCopiesHistoryIntoNewCwd) {
    auto source = m_store.create("/work/a");
    (*source)->appendMessage(user("history"));
    const std::string sourceFile = *(*source)->sessionFile();
    auto forked = m_store.forkFrom(sourceFile, "/work/b");
    ASSERT_TRUE(forked.has_value());
    EXPECT_EQ((*forked)->cwd(), "/work/b");
    EXPECT_EQ((*forked)->header()->parentSession, sourceFile);
    EXPECT_EQ((*forked)->entryCount(), 1U);
    EXPECT_NE(*(*forked)->sessionFile(), sourceFile);
    EXPECT_FALSE(m_store.forkFrom("/nope.jsonl", "/work/b").has_value());
}

TEST_F(SessionStoreTest, InMemoryHasNoFile) {
    auto manager = m_store.inMemory("/work/a");
    ASSERT_TRUE(manager.has_value());
    EXPECT_FALSE((*manager)->isPersisted());
}
