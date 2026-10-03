#include <gtest/gtest.h>

import std;
import pi.serve.coding_server_host;
import pi.testing.fake_session_catalog;
import pi.testing.scripted_server_host;

class OneSessionServices : public IServerServiceHost {
public:
    Result<std::unique_ptr<IServiceAttachment>> attachClient(IServerPresentation&, const ServiceContext&) override {
        return std::unexpected(Error{"unused", "unused"});
    }
};

class RecordingOpener : public ISessionOpener {
public:
    Result<std::shared_ptr<IRoutedSessionHandle>> open(const SessionRecord& record, const ServiceContext&) override {
        m_opened.push_back(record.id + "@" + record.directory);
        if (m_failure) {
            return std::unexpected(*m_failure);
        }
        return std::shared_ptr<IRoutedSessionHandle>(std::make_shared<ScriptedSessionHandle>(std::make_shared<ScriptedSessionState>()));
    }

    std::vector<std::string> m_opened;
    std::optional<Error> m_failure;
};

class CodingServerHostTest : public ::testing::Test {
protected:
    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    FakeSessionCatalog m_catalog;
    OneSessionServices m_services;
    RecordingOpener m_opener;
    CodingServerHost m_host{m_services, m_catalog, m_opener};
};

TEST_F(CodingServerHostTest, ExposesTheServerServices) {
    EXPECT_EQ(&m_host.serverServices(), &m_services);
}

TEST_F(CodingServerHostTest, ResolvesPrefixesToCanonicalIds) {
    m_catalog.create(std::string("session-abc"));
    EXPECT_EQ(*m_host.resolveSession("session-a", context()), "session-abc");
    EXPECT_EQ(m_host.resolveSession("nope", context()).error().code, "session_not_found");
}

TEST_F(CodingServerHostTest, OpensTheCatalogedSession) {
    m_catalog.create(std::string("alpha"));
    ASSERT_TRUE(m_host.openSession("alp", context()));
    EXPECT_EQ(m_opener.m_opened, (std::vector<std::string>{"alpha@/sessions/alpha"}));
}

TEST_F(CodingServerHostTest, OpenFailuresAndUnknownSessionsAreReported) {
    EXPECT_EQ(m_host.openSession("ghost", context()).error().code, "session_not_found");
    m_catalog.create(std::string("alpha"));
    m_opener.m_failure = Error{"missing_cwd", "gone"};
    EXPECT_EQ(m_host.openSession("alpha", context()).error().code, "missing_cwd");
}
