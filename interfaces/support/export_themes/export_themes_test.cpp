#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

import std;
import pi.support.export_themes;
import pi.testing.fake_file_system;

class ExportThemesTest : public testing::Test {
protected:
    ExportThemesTest() {
        m_files.createDirectories("/agent/themes");
        m_files.createDirectories("/project/.pi/themes");
    }

    /** A valid theme file: every required token one color. */
    std::string themeFile(const std::string& name, const std::string& color) {
        Json colors = Json::object();
        for (const char* token : {"accent",
                                  "border",
                                  "borderAccent",
                                  "borderMuted",
                                  "success",
                                  "error",
                                  "warning",
                                  "muted",
                                  "dim",
                                  "text",
                                  "thinkingText",
                                  "selectedBg",
                                  "userMessageBg",
                                  "userMessageText",
                                  "customMessageBg",
                                  "customMessageText",
                                  "customMessageLabel",
                                  "toolPendingBg",
                                  "toolSuccessBg",
                                  "toolErrorBg",
                                  "toolTitle",
                                  "toolOutput",
                                  "mdHeading",
                                  "mdLink",
                                  "mdLinkUrl",
                                  "mdCode",
                                  "mdCodeBlock",
                                  "mdCodeBlockBorder",
                                  "mdQuote",
                                  "mdQuoteBorder",
                                  "mdHr",
                                  "mdListBullet",
                                  "toolDiffAdded",
                                  "toolDiffRemoved",
                                  "toolDiffContext",
                                  "syntaxComment",
                                  "syntaxKeyword",
                                  "syntaxFunction",
                                  "syntaxVariable",
                                  "syntaxString",
                                  "syntaxNumber",
                                  "syntaxType",
                                  "syntaxOperator",
                                  "syntaxPunctuation",
                                  "thinkingOff",
                                  "thinkingMinimal",
                                  "thinkingLow",
                                  "thinkingMedium",
                                  "thinkingHigh",
                                  "thinkingXhigh",
                                  "bashMode"}) {
            colors[token] = color;
        }
        return Json{{"name", name}, {"colors", colors}}.dump();
    }

    FakeFileSystem m_files;
    const std::string m_builtin =
        R"({"dark":{"appearance":"dark","colors":{"text":"#fff"},"export":{}},"light":{"appearance":"light","colors":{"text":"#000"},"export":{}}})";
};

TEST_F(ExportThemesTest, BuiltInThemesComeFirst) {
    m_files.writeFile("/agent/themes/dark.json", themeFile("dark", "#123456"));
    ExportThemes themes(m_builtin, m_files, {"/agent/themes"});
    EXPECT_EQ((*themes.find("dark"))["colors"]["text"], "#fff");
}

TEST_F(ExportThemesTest, FindsCustomThemesByFileNameThenByNameInEachDirectoryInOrder) {
    m_files.writeFile("/agent/themes/ocean.json", themeFile("ocean", "#336699"));
    m_files.writeFile("/agent/themes/other.json", themeFile("sunset", "#cc6633"));
    m_files.writeFile("/project/.pi/themes/ocean.json", themeFile("ocean", "#000000"));
    m_files.writeFile("/project/.pi/themes/forest.json", themeFile("forest", "#228833"));
    ExportThemes themes(m_builtin, m_files, {"/agent/themes", "/project/.pi/themes"});
    EXPECT_EQ((*themes.find("ocean"))["colors"]["accent"], "#336699");
    EXPECT_EQ((*themes.find("sunset"))["colors"]["accent"], "#cc6633")
        << "by the name inside the file";
    EXPECT_EQ((*themes.find("forest"))["colors"]["accent"], "#228833");
    EXPECT_EQ(themes.available(), "dark, light, ocean, sunset, forest");
}

TEST_F(ExportThemesTest, RefusesUnknownAndInvalidThemes) {
    m_files.writeFile("/agent/themes/broken.json", R"({"name":"broken","colors":{}})");
    ExportThemes themes(m_builtin, m_files, {"/agent/themes"});
    const auto unknown = themes.find("nope");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_NE(unknown.error().message.find("available: dark, light, broken"), std::string::npos);
    const auto broken = themes.find("broken");
    ASSERT_FALSE(broken.has_value());
    EXPECT_NE(broken.error().message.find("Missing required color tokens"), std::string::npos);
    EXPECT_FALSE(themes.find("a/b").has_value());
}

TEST_F(ExportThemesTest, ReportsThemeFilesThatAreNotThemes) {
    m_files.writeFile("/agent/themes/list.json", "[]");
    m_files.writeFile("/agent/themes/number.json", "42");
    m_files.writeFile("/agent/themes/broken.json", "{ nope");
    m_files.writeFile("/agent/themes/good.json", themeFile("good", "#123456"));
    ExportThemes themes(m_builtin, m_files, {"/agent/themes"});
    const auto broken = themes.find("broken");
    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().message,
              "Failed to parse theme /agent/themes/broken.json: not valid JSON");
    EXPECT_EQ(themes.find("list").error().message,
              "Failed to parse theme /agent/themes/list.json: not a JSON object");
    EXPECT_TRUE(themes.find("good").has_value()) << "a stray file does not hide the valid ones";
    EXPECT_FALSE(themes.find("absent").has_value());
    EXPECT_EQ(themes.available(), "dark, light, good");
}

TEST_F(ExportThemesTest, ReadsEachDirectoryOnce) {
    m_files.writeFile("/agent/themes/a.json", themeFile("a", "#111111"));
    ExportThemes themes(m_builtin, m_files, {"/agent/themes"});
    EXPECT_FALSE(themes.find("zzz").has_value());
    m_files.writeFile("/agent/themes/zzz.json", themeFile("zzz", "#111111"));
    EXPECT_FALSE(themes.find("zzz").has_value()) << "the scan is cached for the life of the object";
}

TEST_F(ExportThemesTest, AcceptsAByteOrderMark) {
    m_files.writeFile("/agent/themes/bom.json", "\xEF\xBB\xBF" + themeFile("bom", "#444444"));
    ExportThemes themes(m_builtin, m_files, {"/agent/themes"});
    EXPECT_TRUE(themes.find("bom").has_value());
}
