#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.export_theme_resolver;

class ExportThemeResolverTest : public testing::Test {
protected:
    /** A valid theme: every required token set to one color. */
    Json theme(const std::string& color = "#808080") {
        Json colors = Json::object();
        for (const char* token : {"accent", "border", "borderAccent", "borderMuted", "success", "error", "warning", "muted", "dim", "text", "thinkingText",
                                  "selectedBg", "userMessageBg", "userMessageText", "customMessageBg", "customMessageText", "customMessageLabel",
                                  "toolPendingBg", "toolSuccessBg", "toolErrorBg", "toolTitle", "toolOutput", "mdHeading", "mdLink", "mdLinkUrl", "mdCode",
                                  "mdCodeBlock", "mdCodeBlockBorder", "mdQuote", "mdQuoteBorder", "mdHr", "mdListBullet", "toolDiffAdded", "toolDiffRemoved",
                                  "toolDiffContext", "syntaxComment", "syntaxKeyword", "syntaxFunction", "syntaxVariable", "syntaxString", "syntaxNumber",
                                  "syntaxType", "syntaxOperator", "syntaxPunctuation", "thinkingOff", "thinkingMinimal", "thinkingLow", "thinkingMedium",
                                  "thinkingHigh", "thinkingXhigh", "bashMode"}) {
            colors[token] = color;
        }
        return Json{{"name", "t"}, {"colors", colors}};
    }

    ExportThemeResolver m_resolver;
};

TEST_F(ExportThemeResolverTest, MatchesTheTypeScriptResolutionOfCustomThemes) {
    std::ifstream in("src/testing/ts_golden/ts_theme_golden.json");
    ASSERT_TRUE(in.good());
    const Json cases = Json::parse(in);
    ASSERT_GE(cases.size(), 9U);
    for (const Json& expected : cases) {
        const std::string name = expected["theme"]["name"];
        const auto resolved = m_resolver.resolve(name, expected["theme"]);
        ASSERT_TRUE(resolved.has_value()) << name << ": " << resolved.error().message;
        EXPECT_EQ((*resolved)["appearance"], expected["appearance"]) << name;
        EXPECT_EQ((*resolved)["export"], expected["export"]) << name;
        ASSERT_EQ((*resolved)["colors"].size(), expected["colors"].size()) << name;
        // Same tokens, same values and the same order (the order is the page's CSS variable order).
        auto actual = (*resolved)["colors"].items().begin();
        for (const auto& entry : expected["colors"].items()) {
            EXPECT_EQ(actual.key(), entry.key()) << name;
            EXPECT_EQ(actual.value(), entry.value()) << name << " " << entry.key();
            ++actual;
        }
    }
}

TEST_F(ExportThemeResolverTest, DetectsTheAppearanceFromTheColorsUnlessDeclared) {
    Json dark = theme("#e0e0e0");
    for (const char* bg : {"selectedBg", "userMessageBg", "customMessageBg", "toolPendingBg", "toolSuccessBg", "toolErrorBg"}) {
        dark["colors"][bg] = "#101010";
    }
    EXPECT_EQ((*m_resolver.resolve("t", dark))["appearance"], "dark");
    EXPECT_EQ((*m_resolver.resolve("t", theme("#101010")))["appearance"], "light") << "equally light foregrounds and backgrounds count as light, as in TypeScript";
    Json declared = dark;
    declared["appearance"] = "light";
    EXPECT_EQ((*m_resolver.resolve("t", declared))["appearance"], "light");
    Json mixed = theme("#f0f0f0");
    mixed["colors"]["text"] = "#202020";
    for (const char* bg : {"selectedBg", "userMessageBg", "customMessageBg", "toolPendingBg", "toolSuccessBg", "toolErrorBg"}) {
        mixed["colors"][bg] = "#ffffff";
    }
    EXPECT_EQ((*m_resolver.resolve("t", mixed))["appearance"], "light");
}

TEST_F(ExportThemeResolverTest, FallsBackForOptionalTokensAndFillsTerminalDefaults) {
    Json file = theme("#336699");
    file["colors"]["muted"] = "#aaaaaa";
    file["colors"]["text"] = "";
    const Json resolved = *m_resolver.resolve("t", file);
    EXPECT_EQ(resolved["colors"]["scrollbarTrack"], "#aaaaaa");
    EXPECT_EQ(resolved["colors"]["scrollbarThumb"], resolved["colors"]["text"]);
    EXPECT_EQ(resolved["colors"]["thinkingMax"], "#336699");
    EXPECT_EQ(resolved["colors"]["searchMatchBg"], "#336699");
    EXPECT_EQ(resolved["colors"]["text"], "#e5e5e7") << "a dark theme's terminal default foreground";
}

TEST_F(ExportThemeResolverTest, ReportsMissingTokensBadReferencesAndBadColors) {
    EXPECT_FALSE(m_resolver.resolve("t", Json::array()).has_value());
    Json missing = theme();
    missing["colors"].erase("accent");
    const auto noAccent = m_resolver.resolve("t", missing);
    ASSERT_FALSE(noAccent.has_value());
    EXPECT_NE(noAccent.error().message.find("  - accent"), std::string::npos);

    Json unknown = theme();
    unknown["colors"]["accent"] = "nowhere";
    EXPECT_NE(m_resolver.resolve("t", unknown).error().message.find("Variable reference not found: nowhere"), std::string::npos);

    Json circular = theme();
    circular["vars"] = Json{{"a", "b"}, {"b", "a"}};
    circular["colors"]["accent"] = "a";
    EXPECT_NE(m_resolver.resolve("t", circular).error().message.find("Circular variable reference detected"), std::string::npos);

    Json bad = theme();
    bad["colors"]["accent"] = "oklch(5 0 0)";
    EXPECT_NE(m_resolver.resolve("t", bad).error().message.find("Invalid color value"), std::string::npos);
}
