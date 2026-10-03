#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_management_service;
import pi.testing.fake_session_catalog;

class RecordingPresentation : public IServerPresentation {
public:
    Result<void> attachSession(const std::string& sessionId, const ServiceContext&) override {
        m_calls.push_back("attach " + sessionId);
        return m_failure ? Result<void>(std::unexpected(*m_failure)) : Result<void>();
    }
    Result<void> detachSession(const ServiceContext&) override {
        m_calls.push_back("detach");
        return {};
    }
    Result<void> prepareSessionRemoval(const std::string& sessionId, const ServiceContext&) override {
        m_calls.push_back("prepare " + sessionId);
        return {};
    }

    std::vector<std::string> m_calls;
    std::optional<Error> m_failure;
};

class SessionManagementServiceTest : public ::testing::Test {
protected:
    SessionManagementServiceTest() {
        m_methods = m_service.methods();
    }

    Result<std::optional<Json>> call(const std::string& method, const std::vector<Json>& args = {}) {
        return m_methods.at(method)(args, ServiceContext{std::make_shared<AbortSignal>()});
    }

    Json directory() {
        return m_directory.states().at("state")->snapshot().value;
    }

    FakeSessionCatalog m_catalog;
    SessionDirectoryService m_directory{m_catalog, "server"};
    RecordingPresentation m_presentation;
    SessionManagementService m_service{m_presentation, m_catalog, m_directory};
    std::map<std::string, IRemoteService::Method> m_methods;
};

TEST_F(SessionManagementServiceTest, CreateAddsASessionAndUpdatesTheDirectory) {
    const auto created = call("create", {Json::object()});
    ASSERT_TRUE(created);
    ASSERT_TRUE(created->has_value());
    EXPECT_EQ((**created)["sessionId"], "s1");
    EXPECT_EQ((**created)["serverId"], "server");
    EXPECT_EQ(directory()["sessions"].size(), 1u);
    EXPECT_EQ(directory()["revision"], 2);
}

TEST_F(SessionManagementServiceTest, CreateAcceptsAnExplicitId) {
    const auto created = call("create", {Json{{"id", "mine"}}});
    ASSERT_TRUE(created);
    EXPECT_EQ((**created)["sessionId"], "mine");
    EXPECT_EQ(call("create", {Json{{"id", "mine"}}}).error().code, "session_exists");
    EXPECT_EQ(call("create", {Json{{"id", 3}}}).error().code, "invalid_request");
    EXPECT_EQ(call("create", {}).error().code, "invalid_request");
}

TEST_F(SessionManagementServiceTest, RemoveReleasesTheSessionBeforeDeletingIt) {
    call("create", {Json{{"id", "alpha"}}});
    ASSERT_TRUE(call("remove", {Json("alp")}));
    EXPECT_EQ(m_presentation.m_calls, (std::vector<std::string>{"prepare alpha"}));
    EXPECT_EQ(m_catalog.removed(), 1);
    EXPECT_TRUE(directory()["sessions"].empty());
}

TEST_F(SessionManagementServiceTest, RemovingAnUnknownSessionFails) {
    EXPECT_EQ(call("remove", {Json("ghost")}).error().code, "session_not_found");
    EXPECT_TRUE(m_presentation.m_calls.empty());
    EXPECT_EQ(call("remove", {}).error().code, "invalid_request");
    EXPECT_EQ(call("remove", {Json("")}).error().code, "invalid_request");
}

TEST_F(SessionManagementServiceTest, AttachAndDetachGoThroughThePresentation) {
    ASSERT_TRUE(call("attach", {Json("s1")}));
    ASSERT_TRUE(call("detach"));
    EXPECT_EQ(m_presentation.m_calls, (std::vector<std::string>{"attach s1", "detach"}));
    m_presentation.m_failure = Error{"session_not_found", "Session was not found"};
    EXPECT_EQ(call("attach", {Json("zz")}).error().code, "session_not_found");
}

TEST_F(SessionManagementServiceTest, HasNoState) {
    EXPECT_TRUE(m_service.states().empty());
}
