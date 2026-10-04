#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_export_data;
import pi.testing.session_harness;

TEST(SessionExportDataTest, CarriesTheHeaderTheEntriesAndTheLeaf) {
    SessionHarness harness("/work");
    UserMessage user;
    user.content = std::string("hello");
    harness.session().appendMessage(user);
    const SessionExportData data;

    const Json file = data.build(harness.session(), std::nullopt, std::nullopt);
    EXPECT_EQ(file["header"]["cwd"], "/work");
    ASSERT_EQ(file["entries"].size(), 1U);
    EXPECT_EQ(file["entries"][0]["type"], "message");
    EXPECT_EQ(file["leafId"], file["entries"][0]["id"]);
    EXPECT_FALSE(file.contains("systemPrompt"));
    EXPECT_FALSE(file.contains("tools"));

    const Json live = data.build(harness.session(), "be brief", Json::array({Json{{"name", "read"}}}));
    EXPECT_EQ(live["systemPrompt"], "be brief");
    EXPECT_EQ(live["tools"][0]["name"], "read");
}
