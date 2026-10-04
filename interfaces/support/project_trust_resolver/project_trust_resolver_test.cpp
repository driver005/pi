#include <gtest/gtest.h>

import std;
import pi.support.project_trust_resolver;
import pi.testing.fake_file_system;

class MapTrustStore : public IProjectTrustStore {
public:
    Result<std::optional<bool>> get(const std::string& cwd) override {
        const auto found = m_values.find(cwd);
        return found == m_values.end() ? std::optional<bool>() : std::optional<bool>(found->second);
    }
    Result<std::optional<ProjectTrustEntry>> getEntry(const std::string&) override {
        return std::optional<ProjectTrustEntry>();
    }
    Result<void> set(const std::string& cwd, std::optional<bool> decision) override {
        if (decision) {
            m_values[cwd] = *decision;
        } else {
            m_values.erase(cwd);
        }
        return {};
    }
    Result<void> setMany(const std::vector<ProjectTrustUpdate>&) override { return {}; }

private:
    std::map<std::string, bool> m_values;
};

class ProjectTrustResolverTest : public testing::Test {
protected:
    ProjectTrustResolverTest() { m_files.createDirectories("/work/p/.pi/extensions"); }

    FakeFileSystem m_files;
    ProjectTrustProbe m_probe{m_files};
    MapTrustStore m_store;
    ProjectTrustResolver m_resolver{m_store, m_probe};
};

TEST_F(ProjectTrustResolverTest, OverrideWins) {
    EXPECT_EQ(*m_resolver.resolve("/work/p", true, "never", nullptr), true);
    EXPECT_EQ(*m_resolver.resolve("/work/p", false, "always", nullptr), false);
}

TEST_F(ProjectTrustResolverTest, NothingToGateIsTrusted) {
    m_files.createDirectories("/work/plain");
    EXPECT_TRUE(*m_resolver.resolve("/work/plain", std::nullopt, "never", nullptr));
}

TEST_F(ProjectTrustResolverTest, PluginAnswerBeatsStoreAndCanBeRemembered) {
    m_store.set("/work/p", false);
    const auto trusted = m_resolver.resolve("/work/p", std::nullopt, "ask", [](const std::string&) {
        return std::optional<std::pair<bool, bool>>({true, true});
    });
    EXPECT_TRUE(*trusted);
    EXPECT_EQ(*m_store.get("/work/p"), std::optional<bool>(true));
    const auto abstained = m_resolver.resolve("/work/p", std::nullopt, "ask", [](const std::string&) {
        return std::optional<std::pair<bool, bool>>();
    });
    EXPECT_TRUE(*abstained);
}

TEST_F(ProjectTrustResolverTest, StoredDecisionThenDefaults) {
    EXPECT_FALSE(*m_resolver.resolve("/work/p", std::nullopt, "ask", nullptr));
    EXPECT_TRUE(*m_resolver.resolve("/work/p", std::nullopt, "always", nullptr));
    EXPECT_FALSE(*m_resolver.resolve("/work/p", std::nullopt, "never", nullptr));
    m_store.set("/work/p", true);
    EXPECT_TRUE(*m_resolver.resolve("/work/p", std::nullopt, "never", nullptr));
    m_store.set("/work/p", false);
    EXPECT_FALSE(*m_resolver.resolve("/work/p", std::nullopt, "always", nullptr));
}
