#include <gtest/gtest.h>

import std;
import pi.support.session_branch_serializer;
import pi.testing.fixed_clock;

TEST(SessionBranchSerializerTest, ChainsTheBranchBehindAHeaderAndAddsTrailingEntries) {
    FixedClock clock(0);
    SessionBranchSerializer serializer(clock);
    SessionEntry a;
    a.id = "a";
    a.body = Json{{"type", "message"}, {"id", "a"}, {"parentId", "gone"}};
    SessionEntry b;
    b.id = "b";
    b.body = Json{{"type", "message"}, {"id", "b"}, {"parentId", "a"}};
    const std::string text = serializer.serialize("s1", "/work", {a, b}, [](const std::optional<std::string>& parent, const std::string& timestamp) {
        return std::vector<Json>{Json{{"type", "custom"}, {"parentId", *parent}, {"timestamp", timestamp}}};
    });
    std::vector<Json> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(Json::parse(line));
    }
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[0]["type"], "session");
    EXPECT_EQ(lines[0]["version"], 3);
    EXPECT_EQ(lines[0]["id"], "s1");
    EXPECT_EQ(lines[0]["cwd"], "/work");
    EXPECT_EQ(lines[0]["timestamp"], "1970-01-01T00:00:00.000Z");
    EXPECT_TRUE(lines[1]["parentId"].is_null());
    EXPECT_EQ(lines[2]["parentId"], "a");
    EXPECT_EQ(lines[3]["parentId"], "b");
    EXPECT_EQ(lines[3]["timestamp"], "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(serializer.serialize("s", "/", {}).find("\n"), serializer.serialize("s", "/", {}).size() - 1);
}
