#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;
import pi.support.mcp_tool_namer;

class McpToolNamerTest : public testing::Test {
protected:
    BoringCrypto m_crypto;
    McpToolNamer m_namer{m_crypto};
};

TEST_F(McpToolNamerTest, SanitizesEverythingButWordCharacters) {
    EXPECT_EQ(m_namer.create("docs", "search"), "mcp__docs__search");
    EXPECT_EQ(m_namer.create("my-docs", "search.things/now"), "mcp__my_docs__search_things_now");
}

TEST_F(McpToolNamerTest, TakenNamesGetTheHashSuffix) {
    const auto taken = [](const std::string& name) { return name == "mcp__a__b_c"; };
    EXPECT_EQ(m_namer.create("a", "b-c", taken), "mcp__a__b_c_02654a5c");
    EXPECT_EQ(m_namer.create("a", "b_c"), "mcp__a__b_c");
}

TEST_F(McpToolNamerTest, LongNamesAreShortenedToSixtyFourCharacters) {
    const std::string name = m_namer.create("s", std::string(80, 'x'));
    EXPECT_EQ(name.size(), 64U);
    EXPECT_EQ(name.substr(55), "_cbf4205d");
    EXPECT_EQ(name.substr(0, 10), "mcp__s__xx");
}
