#include <gtest/gtest.h>

import std;
import pi.ai.memory_credential_store;
import pi.testing.fake_environment;
import pi.testing.scripted_process_runner;

class MemoryCredentialStoreTest : public testing::Test {
protected:
    MemoryCredentialStoreTest()
        : m_processes([](const ProcessRequest&) -> Result<ProcessResult> { return ProcessResult{}; }),
          m_resolver(m_environment, m_processes),
          m_store(m_resolver) {}

    FakeEnvironment m_environment{{{"K", "v"}}};
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_resolver;
    MemoryCredentialStore m_store;
};

TEST_F(MemoryCredentialStoreTest, ModifyReadListRemove) {
    Credential credential;
    credential.key = "$K";
    ASSERT_TRUE(m_store.modify("p", [&](const std::optional<Credential>& current) {
        EXPECT_FALSE(current.has_value());
        return Result<std::optional<Credential>>(std::optional<Credential>(credential));
    }));
    EXPECT_EQ((*m_store.read("p"))->key, "v");
    EXPECT_EQ(m_store.list()->size(), 1U);
    ASSERT_TRUE(m_store.remove("p"));
    EXPECT_FALSE((*m_store.read("p")).has_value());
}
