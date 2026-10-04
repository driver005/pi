#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mcp_result_parser;

class McpResultParserTest : public testing::Test {
protected:
    McpResultParser m_parser;
};

TEST_F(McpResultParserTest, InitializeNeedsTheRequiredFields) {
    const Json ok = Json::parse(R"({"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":{"name":"s","version":"1"}})");
    EXPECT_TRUE(m_parser.initialize(ok).has_value());
    Json bad = ok;
    bad["serverInfo"].erase("version");
    EXPECT_FALSE(m_parser.initialize(bad).has_value());
    Json badInstructions = ok;
    badInstructions["instructions"] = 5;
    EXPECT_FALSE(m_parser.initialize(badInstructions).has_value());
    EXPECT_FALSE(m_parser.initialize(Json("x")).has_value());
}

TEST_F(McpResultParserTest, ListPagesValidateItemsAndCursors) {
    std::optional<std::string> cursor;
    const Json page = Json::parse(R"({"tools":[{"name":"a","inputSchema":{}},{"name":"b","inputSchema":{"type":"object"}}],"nextCursor":"c2"})");
    const auto items = m_parser.listPage("tools/list", "tools", "tool", page, cursor);
    ASSERT_TRUE(items.has_value());
    EXPECT_EQ(items->size(), 2U);
    EXPECT_EQ(cursor, "c2");
    for (const std::string empty : {R"("nextCursor":null)", R"("nextCursor":"")"}) {
        EXPECT_TRUE(m_parser.listPage("tools/list", "tools", "tool", Json::parse("{\"tools\":[]," + empty + "}"), cursor).has_value());
        EXPECT_FALSE(cursor.has_value());
    }
    EXPECT_FALSE(m_parser.listPage("tools/list", "tools", "tool", Json::parse(R"({"tools":[{"name":"a"}]})"), cursor).has_value());
    EXPECT_FALSE(m_parser.listPage("tools/list", "tools", "tool", Json::parse(R"({"tools":[],"nextCursor":5})"), cursor).has_value());
    EXPECT_FALSE(m_parser.listPage("tools/list", "tools", "tool", Json::parse(R"({"nope":[]})"), cursor).has_value());
}

TEST_F(McpResultParserTest, ResourcesWithoutANameUseTheirUri) {
    std::optional<std::string> cursor;
    const auto resources = m_parser.listPage("resources/list", "resources", "resource",
                                             Json::parse(R"({"resources":[{"uri":"file:///a"},{"uri":"file:///b","name":"B"}]})"), cursor);
    ASSERT_TRUE(resources.has_value());
    EXPECT_EQ((*resources)[0]["name"], "file:///a");
    EXPECT_EQ((*resources)[1]["name"], "B");
    const auto templates = m_parser.listPage("resources/templates/list", "resourceTemplates", "resourceTemplate",
                                             Json::parse(R"({"resourceTemplates":[{"uriTemplate":"file:///{x}"}]})"), cursor);
    ASSERT_TRUE(templates.has_value());
    EXPECT_EQ((*templates)[0]["name"], "file:///{x}");
}

TEST_F(McpResultParserTest, ToolConversion) {
    const McpTool tool = m_parser.tool(Json::parse(R"({"name":"t","title":"T","description":"d","inputSchema":{"type":"object"},
        "outputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}})"));
    EXPECT_EQ(tool.name, "t");
    EXPECT_EQ(tool.title, "T");
    EXPECT_EQ(tool.description, "d");
    EXPECT_EQ(tool.outputSchema["type"], "object");
    EXPECT_EQ(tool.annotations["readOnlyHint"], true);
    const McpTool bare = m_parser.tool(Json::parse(R"({"name":"t","inputSchema":{}})"));
    EXPECT_TRUE(bare.outputSchema.is_null());
    EXPECT_TRUE(bare.annotations.is_null());
    EXPECT_FALSE(bare.description.has_value());
}

TEST_F(McpResultParserTest, CallResults) {
    const auto ok = m_parser.callResult(Json::parse(R"({"content":[{"type":"text","text":"hi"}],"structuredContent":{"a":1},"isError":true})"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->content.size(), 1U);
    EXPECT_EQ(ok->structuredContent["a"], 1);
    EXPECT_TRUE(ok->isError);
    const auto bare = m_parser.callResult(Json::parse(R"({"structuredContent":{"a":1}})"));
    ASSERT_TRUE(bare.has_value());
    EXPECT_TRUE(bare->content.empty());
    EXPECT_FALSE(m_parser.callResult(Json::parse(R"({"content":5})")).has_value());
    EXPECT_FALSE(m_parser.callResult(Json::parse(R"({"content":[],"structuredContent":[]})")).has_value());
    EXPECT_FALSE(m_parser.callResult(Json("x")).has_value());
}

TEST_F(McpResultParserTest, ReadResults) {
    EXPECT_TRUE(m_parser.readResult(Json::parse(R"({"contents":[{"uri":"u","text":"t"},{"uri":"v","blob":"QQ=="}]})")).has_value());
    EXPECT_FALSE(m_parser.readResult(Json::parse(R"({"contents":[{"uri":"u"}]})")).has_value());
    EXPECT_FALSE(m_parser.readResult(Json::parse(R"({"nope":1})")).has_value());
}
