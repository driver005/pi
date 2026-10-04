#include <gtest/gtest.h>
#include <cstdlib>

import std;
import pi.coding_runtime_factory;

class CodingRuntimeFactoryTest : public testing::Test {
protected:
    CodingRuntimeFactoryTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/coding_runtime_factory";
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/project");
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    SessionRuntimeRequest request(const std::string& cwd, bool withManager) {
        SessionRuntimeRequest out;
        out.cwd = cwd;
        out.agentDir = m_dir + "/agent";
        if (withManager) {
            out.sessionManager = std::move(*m_services->sessions().inMemory(cwd));
        }
        return out;
    }

    std::string m_dir;
    std::unique_ptr<CodingServices> m_services;
};

TEST_F(CodingRuntimeFactoryTest, BuildsASessionAroundTheTree) {
    CodingRuntimeFactory factory(*m_services, {});
    auto handle = factory.create(request(m_dir + "/project", true));
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ((*handle)->cwd(), m_dir + "/project");
    EXPECT_EQ((*handle)->session().model().id, "faux-1");
}

TEST_F(CodingRuntimeFactoryTest, RejectsMissingManagerAndMissingCwd) {
    CodingRuntimeFactory factory(*m_services, {});
    EXPECT_EQ(factory.create(request(m_dir + "/project", false)).error().code, "invalid_request");
    EXPECT_EQ(factory.create(request(m_dir + "/gone", true)).error().code, "missing_cwd");
}
