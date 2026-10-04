#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.support.html_exporter;

class HtmlExporterTest : public testing::Test {
protected:
    ExportAssets assets() {
        ExportAssets out;
        out.templateHtml = "<style>{{CSS}}</style><script id=\"d\">{{SESSION_DATA}}</script><script>{{MARKED_JS}}</script><script>{{HIGHLIGHT_JS}}</script><script>{{JS}}</script>";
        out.templateCss = ":root{ {{THEME_VARS}} } body{background:{{BODY_BG}}} .c{background:{{CONTAINER_BG}}} .i{background:{{INFO_BG}}}";
        out.templateJs = "var a = '$&'; // {{SESSION_DATA}} stays";
        out.markedJs = "marked();";
        out.highlightJs = "hljs();";
        out.themesJson = R"({"dark":{"appearance":"dark","colors":{"text":"#ffffff","accent":"#a798d7"},"export":{"pageBg":"#111111","cardBg":"#222222","infoBg":"#333333"}},
                             "light":{"appearance":"light","colors":{"text":"#000000"},"export":{}}})";
        return out;
    }

    Base64Codec m_base64;
};

TEST_F(HtmlExporterTest, FillsTheTemplateWithThemeColorsAndTheEncodedSession) {
    const HtmlExporter exporter(assets(), m_base64);
    const Json session{{"header", Json{{"id", "s1"}}}, {"entries", Json::array()}, {"leafId", nullptr}, {"note", "caf\xC3\xA9"}};
    const auto html = exporter.render(session, "dark");
    ASSERT_TRUE(html.has_value());
    EXPECT_NE(html->find("--text: #ffffff;"), std::string::npos);
    EXPECT_NE(html->find("--accent: #a798d7;"), std::string::npos);
    EXPECT_NE(html->find("--exportPageBg: #111111;"), std::string::npos);
    EXPECT_NE(html->find("body{background:#111111}"), std::string::npos);
    EXPECT_NE(html->find(".c{background:#222222}"), std::string::npos);
    EXPECT_NE(html->find(".i{background:#333333}"), std::string::npos);
    EXPECT_NE(html->find("<script>marked();</script><script>hljs();</script>"), std::string::npos);

    const std::size_t start = html->find("<script id=\"d\">") + 15;
    const std::string encoded = html->substr(start, html->find("</script>", start) - start);
    const auto decoded = m_base64.decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(Json::parse(*decoded), session);
}

TEST_F(HtmlExporterTest, TextInsideAReplacementIsNeverTakenForAPlaceholder) {
    const HtmlExporter exporter(assets(), m_base64);
    const auto html = exporter.render(Json::object(), "dark");
    ASSERT_TRUE(html.has_value());
    EXPECT_NE(html->find("var a = '$&'; // {{SESSION_DATA}} stays"), std::string::npos) << "the script keeps its own placeholder-looking text and its $&";
    std::size_t count = 0;
    for (std::size_t at = html->find("{{SESSION_DATA}}"); at != std::string::npos; at = html->find("{{SESSION_DATA}}", at + 1)) {
        ++count;
    }
    EXPECT_EQ(count, 1U);
}

TEST_F(HtmlExporterTest, DefaultsToDarkAndFallsBackForMissingExportColors) {
    const HtmlExporter exporter(assets(), m_base64);
    const auto byDefault = exporter.render(Json::object(), "");
    ASSERT_TRUE(byDefault.has_value());
    EXPECT_NE(byDefault->find("--exportPageBg: #111111;"), std::string::npos);
    const auto light = exporter.render(Json::object(), "light");
    ASSERT_TRUE(light.has_value());
    EXPECT_NE(light->find("--exportPageBg: rgb(24, 24, 30);"), std::string::npos);
}

TEST_F(HtmlExporterTest, UnknownThemesAreRefusedWithTheAvailableOnes) {
    const HtmlExporter exporter(assets(), m_base64);
    const auto html = exporter.render(Json::object(), "solarized");
    ASSERT_FALSE(html.has_value());
    EXPECT_EQ(html.error().code, "unknown_theme");
    EXPECT_EQ(html.error().message, "Unknown theme \"solarized\" (available: dark, light)");
}
