#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.embedded_export_assets;
import pi.types.json;

TEST(EmbeddedExportAssetsTest, CarriesTheTemplateTheLibrariesAndTheBuiltInThemes) {
    const EmbeddedExportAssets embedded;
    const ExportAssets assets = embedded.assets();
    for (const char* placeholder : {"{{CSS}}", "{{JS}}", "{{SESSION_DATA}}", "{{MARKED_JS}}", "{{HIGHLIGHT_JS}}"}) {
        EXPECT_NE(assets.templateHtml.find(placeholder), std::string::npos) << placeholder;
    }
    for (const char* placeholder : {"{{THEME_VARS}}", "{{BODY_BG}}", "{{CONTAINER_BG}}", "{{INFO_BG}}"}) {
        EXPECT_NE(assets.templateCss.find(placeholder), std::string::npos) << placeholder;
    }
    EXPECT_NE(assets.templateJs.find("session-data"), std::string::npos);
    EXPECT_GT(assets.markedJs.size(), 10000U);
    EXPECT_GT(assets.highlightJs.size(), 100000U);
    const Json themes = Json::parse(assets.themesJson);
    for (const char* name : {"dark", "light"}) {
        ASSERT_TRUE(themes.contains(name)) << name;
        EXPECT_TRUE(themes[name]["colors"].contains("userMessageBg")) << name;
        EXPECT_TRUE(themes[name]["export"].contains("pageBg")) << name;
    }
}
