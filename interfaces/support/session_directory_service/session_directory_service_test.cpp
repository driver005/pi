#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_directory_service;
import pi.testing.fake_session_catalog;

class SessionDirectoryServiceTest : public ::testing::Test {
protected:
    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    FakeSessionCatalog m_catalog;
};

TEST_F(SessionDirectoryServiceTest, StartsWithTheCatalogSessions) {
    m_catalog.create("a");
    SessionDirectoryService service(m_catalog, "server");
    const Json value = service.states().at("state")->snapshot().value;
    EXPECT_EQ(value["revision"], 1);
    ASSERT_EQ(value["sessions"].size(), 1u);
    EXPECT_EQ(value["sessions"][0]["serverId"], "server");
    EXPECT_EQ(value["sessions"][0]["sessionId"], "a");
    EXPECT_EQ(value["sessions"][0]["createdAt"], 1001);
    EXPECT_TRUE(service.methods().empty());
}

TEST_F(SessionDirectoryServiceTest, RefreshPublishesTheNewListWithAHigherRevision) {
    SessionDirectoryService service(m_catalog, "server");
    IReplicatedState* state = service.states().at("state");
    std::vector<std::int64_t> sequences;
    state->subscribe([&](const Json&, std::int64_t sequence, const ServiceContext&) { sequences.push_back(sequence); });
    m_catalog.create("a");
    m_catalog.create("b");
    ASSERT_TRUE(service.refresh(context()));
    const Json value = state->snapshot().value;
    EXPECT_EQ(value["revision"], 2);
    ASSERT_EQ(value["sessions"].size(), 2u);
    EXPECT_EQ(value["sessions"][0]["sessionId"], "b");
    EXPECT_EQ(sequences, (std::vector<std::int64_t>{1}));
}

TEST_F(SessionDirectoryServiceTest, AFailingCatalogLeavesTheStateAlone) {
    m_catalog.create("a");
    SessionDirectoryService service(m_catalog, "server");
    m_catalog.fail(Error{"io", "disk gone"});
    EXPECT_EQ(service.refresh(context()).error().code, "io");
    const Json value = service.states().at("state")->snapshot().value;
    EXPECT_EQ(value["revision"], 1);
    EXPECT_EQ(value["sessions"].size(), 1u);
}

TEST_F(SessionDirectoryServiceTest, ACatalogThatFailsAtStartGivesAnEmptyDirectory) {
    m_catalog.fail(Error{"io", "disk gone"});
    SessionDirectoryService service(m_catalog, "server");
    EXPECT_TRUE(service.states().at("state")->snapshot().value["sessions"].empty());
}

TEST_F(SessionDirectoryServiceTest, MutationsAreSerialised) {
    SessionDirectoryService service(m_catalog, "server");
    std::unique_lock<std::mutex> lock(service.mutations(), std::try_to_lock);
    EXPECT_TRUE(lock.owns_lock());
    std::atomic<bool> acquired{false};
    std::thread other([&] {
        const std::lock_guard<std::mutex> second(service.mutations());
        acquired = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(acquired.load());
    lock.unlock();
    other.join();
    EXPECT_TRUE(acquired.load());
}
