#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.mcp_result_converter;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class McpResultConverterTest : public testing::Test {
protected:
    McpResultConverterTest() {
        m_files.createDirectories("/tmp");
    }

    McpCallResult result(const Json& content, bool isError = false) {
        McpCallResult out;
        out.content = content;
        out.isError = isError;
        return out;
    }

    Json textBlock(const std::string& text) {
        return Json{{"type", "text"}, {"text", text}};
    }

    std::string textAt(const AgentToolResult& converted, std::size_t index) {
        return std::get<TextContent>(converted.content.at(index)).text;
    }

    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    McpResultConverter m_converter{m_files, m_crypto, m_base64, m_environment};
};

TEST_F(McpResultConverterTest, TextPassesThroughWithDetails) {
    const auto converted = m_converter.convert(
        "docs", "search", result(Json::array({textBlock("one"), textBlock("two")})));
    ASSERT_EQ(converted.content.size(), 2U);
    EXPECT_EQ(textAt(converted, 0), "one");
    EXPECT_EQ(converted.details["server"], "docs");
    EXPECT_EQ(converted.details["tool"], "search");
    EXPECT_FALSE(converted.details.contains("fullOutputPath"));
    EXPECT_FALSE(converted.isError);
    EXPECT_EQ(converted.structuredContent["content"].size(), 2U);
    EXPECT_FALSE(converted.structuredContent.contains("isError"));
}

TEST_F(McpResultConverterTest, StructuredContentStandsInForMissingBlocks) {
    McpCallResult input;
    input.structuredContent = Json{{"a", 1}};
    const auto converted = m_converter.convert("s", "t", input);
    ASSERT_EQ(converted.content.size(), 1U);
    EXPECT_EQ(textAt(converted, 0), "{\n  \"a\": 1\n}");
    EXPECT_EQ(converted.structuredContent["structuredContent"]["a"], 1);
}

TEST_F(McpResultConverterTest, ErrorsKeepContentOrGetAPlaceholder) {
    const auto withText = m_converter.convert("s", "t", result(Json::array({textBlock("bad")}), true));
    EXPECT_TRUE(withText.isError);
    EXPECT_EQ(textAt(withText, 0), "bad");
    EXPECT_TRUE(withText.structuredContent["isError"].get<bool>());
    const auto bare = m_converter.convert("srv", "tool", result(Json::array(), true));
    EXPECT_EQ(textAt(bare, 0), "MCP tool srv/tool returned an error");
}

TEST_F(McpResultConverterTest, ConvertsEveryBlockKind) {
    const Json blocks = Json::array({
        Json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}},
        Json{{"type", "audio"}, {"data", "AAAA"}, {"mimeType", "audio/wav"}},
        Json{{"type", "resource_link"}, {"uri", "file:///a.txt"}, {"name", "a"}, {"title", "The A"},
             {"mimeType", "text/plain"}, {"size", 2048}, {"description", "first"}},
        Json{{"type", "resource_link"}, {"uri", "file:///b"}, {"name", "b"}},
        Json{{"type", "resource"}, {"resource", Json{{"uri", "file:///t"}, {"text", "inline"}}}},
        Json{{"type", "resource"}, {"resource", Json{{"uri", "file:///i"}, {"mimeType", "image/jpeg"}, {"blob", "QUJD"}}}},
        Json{{"type", "mystery"}},
    });
    const auto converted = m_converter.convert("s", "t", result(blocks));
    ASSERT_EQ(converted.content.size(), 7U);
    const auto& image = std::get<ImageContent>(converted.content[0]);
    EXPECT_EQ(image.mimeType, "image/png");
    EXPECT_EQ(textAt(converted, 1), "[audio audio/wav omitted]");
    EXPECT_EQ(textAt(converted, 2), "[Resource file:///a.txt \"The A\" (text/plain, 2.0KB): first]");
    EXPECT_EQ(textAt(converted, 3), "[Resource file:///b \"b\"]");
    EXPECT_EQ(textAt(converted, 4), "inline");
    EXPECT_EQ(std::get<ImageContent>(converted.content[5]).data, "QUJD");
    EXPECT_EQ(textAt(converted, 6), "[unsupported MCP content mystery]");
}

TEST_F(McpResultConverterTest, BinaryResourcesAreSavedUnlessTheyAreText) {
    const auto blob = [this](const std::string& uri, const std::string& mime, const std::string& bytes) {
        return Json{{"type", "resource"},
                    {"resource", Json{{"uri", uri}, {"mimeType", mime}, {"blob", m_base64.encode(bytes)}}}};
    };
    const auto converted = m_converter.convert(
        "s", "t", result(Json::array({blob("https://x/files/report.pdf?v=1", "application/pdf", "PDFDATA"),
                                      blob("file:///data", "application/json", "{\"k\":1}"),
                                      blob("file:///noext", "", "zz")})));
    const std::string saved = textAt(converted, 0);
    ASSERT_EQ(saved.rfind("[Binary resource https://x/files/report.pdf?v=1 (application/pdf, 7B) saved to /tmp/pi-mcp-", 0), 0U);
    const std::string path = saved.substr(saved.find("/tmp/"), saved.size() - saved.find("/tmp/") - 1);
    EXPECT_TRUE(path.ends_with(".pdf"));
    EXPECT_EQ(m_files.content(path), "PDFDATA");
    EXPECT_EQ(textAt(converted, 1), "{\"k\":1}");
    EXPECT_NE(textAt(converted, 2).find("(unknown type, 2B) saved to /tmp/pi-mcp-"), std::string::npos);
    EXPECT_TRUE(textAt(converted, 2).find(".bin]") != std::string::npos);
}

TEST_F(McpResultConverterTest, LongTextKeepsStartAndEndAndSavesTheFullText) {
    const std::string full = std::string(15000, 'a') + std::string(15000, 'z');
    const Json blocks = Json::array({textBlock(full), Json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}});
    const auto converted = m_converter.convert("s", "t", result(blocks));
    ASSERT_EQ(converted.content.size(), 2U);
    const std::string text = textAt(converted, 0);
    EXPECT_EQ(text.rfind("Warning: truncated output (original token count: 7500)\nTotal output lines: 1\n\n", 0), 0U);
    EXPECT_NE(text.find("\xE2\x80\xA6" "9520 chars truncated\xE2\x80\xA6"), std::string::npos);
    EXPECT_TRUE(std::holds_alternative<ImageContent>(converted.content[1]));
    const std::string path = converted.details["fullOutputPath"];
    EXPECT_NE(text.find("[Full output: " + path + " (read it with offset/limit)]"), std::string::npos);
    EXPECT_EQ(m_files.content(path), full);
}

TEST_F(McpResultConverterTest, UnsavableOutputIsReported) {
    FakeFileSystem empty;
    McpResultConverter converter(empty, m_crypto, m_base64, m_environment);
    const auto converted = converter.convert("s", "t", result(Json::array({textBlock(std::string(30000, 'x'))})));
    EXPECT_NE(textAt(converted, 0).find("[Could not save the full output: "), std::string::npos);
    EXPECT_FALSE(converted.details.contains("fullOutputPath"));
}
