#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;
import pi.support.server_identity;
import pi.testing.fake_file_system;

class ServerIdentityTest : public ::testing::Test {
protected:
    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    ServerIdentity m_identity{m_files, m_crypto};
};

TEST_F(ServerIdentityTest, GeneratedIdsAreCanonicalUuidV4) {
    for (int i = 0; i < 50; ++i) {
        const std::string id = m_identity.generate();
        EXPECT_TRUE(m_identity.valid(id)) << id;
    }
    EXPECT_NE(m_identity.generate(), m_identity.generate());
}

TEST_F(ServerIdentityTest, ValidationRejectsEverythingElse) {
    EXPECT_TRUE(m_identity.valid("00000000-0000-4000-8000-000000000001"));
    EXPECT_FALSE(m_identity.valid(""));
    EXPECT_FALSE(m_identity.valid("00000000-0000-4000-8000-00000000000G"));
    EXPECT_FALSE(m_identity.valid("00000000-0000-1000-8000-000000000001"));
    EXPECT_FALSE(m_identity.valid("00000000-0000-4000-c000-000000000001"));
    EXPECT_FALSE(m_identity.valid("00000000-0000-4000-8000-00000000000A"));
    EXPECT_FALSE(m_identity.valid("0000000000004000800000000000000001"));
}

TEST_F(ServerIdentityTest, ARequestedIdIsUsedWhenValid) {
    EXPECT_EQ(*m_identity.resolve("/srv", std::string("00000000-0000-4000-8000-000000000001")),
              "00000000-0000-4000-8000-000000000001");
    EXPECT_EQ(m_identity.resolve("/srv", std::string("nope")).error().code, "invalid_server_id");
    EXPECT_FALSE(m_files.exists("/srv"));
}

TEST_F(ServerIdentityTest, TheDefaultIdentityIsCreatedOnceAndKept) {
    const auto first = m_identity.resolve("/srv/server", std::nullopt);
    ASSERT_TRUE(first);
    EXPECT_TRUE(m_identity.valid(*first));
    EXPECT_EQ(m_files.content("/srv/server/default-server-id"), *first);
    EXPECT_EQ(*m_identity.resolve("/srv/server", std::nullopt), *first);
}

TEST_F(ServerIdentityTest, ATrailingNewlineInTheFileIsIgnored) {
    m_files.createDirectories("/srv");
    m_files.writeFile("/srv/default-server-id", "00000000-0000-4000-8000-000000000001\n");
    EXPECT_EQ(*m_identity.resolve("/srv", std::nullopt), "00000000-0000-4000-8000-000000000001");
}

TEST_F(ServerIdentityTest, ACorruptDefaultFileIsAnError) {
    m_files.createDirectories("/srv");
    m_files.writeFile("/srv/default-server-id", "garbage");
    EXPECT_EQ(m_identity.resolve("/srv", std::nullopt).error().code, "invalid_server_id");
}
