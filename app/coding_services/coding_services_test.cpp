#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.coding_services;

class CodingServicesTest : public testing::Test {
protected:
    CodingServicesTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/coding_services";
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
    }

    std::string m_dir;
};

TEST_F(CodingServicesTest, FauxModeOffersTheScriptedModel) {
    CodingServices services(m_dir, m_dir + "/catalog", true);
    const auto available = services.models().models().availableModels();
    ASSERT_EQ(available.size(), 1U);
    EXPECT_EQ(available[0].provider, "faux");
}

TEST_F(CodingServicesTest, SessionStoreCreatesInMemoryAndPersistedSessions) {
    CodingServices services(m_dir, m_dir + "/catalog", false);
    const auto memory = services.sessions().inMemory("/tmp");
    ASSERT_TRUE(memory.has_value());
    const auto persisted = services.sessions().create("/tmp", m_dir + "/sessions", std::nullopt, std::nullopt);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_NE((*persisted)->sessionId(), (*memory)->sessionId());
}

TEST_F(CodingServicesTest, TrustResolverTrustsProjectsWithNothingToGate) {
    CodingServices services(m_dir, m_dir + "/catalog", false);
    const auto trusted = services.trust().resolve(m_dir, std::nullopt, "ask", {});
    ASSERT_TRUE(trusted.has_value());
    EXPECT_TRUE(*trusted);
}
