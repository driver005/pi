#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.message_entry_locator;

TEST(MessageEntryLocatorTest, FindsNewestMatchingEntry) {
    AgentMessageCodec codec;
    UserMessage same;
    same.content = std::string("same");
    same.timestamp = 5;
    UserMessage other;
    other.content = std::string("other");
    auto entry = [&](const std::string& id, const AgentMessage& message) {
        SessionEntry out;
        out.id = id;
        out.type = "message";
        out.body = Json{{"message", codec.toJson(message)}};
        return out;
    };
    const std::vector<SessionEntry> branch = {entry("a", same), entry("b", other), entry("c", same)};
    MessageEntryLocator locator;
    EXPECT_EQ(locator.find(branch, AgentMessage(same)), "c");
    EXPECT_EQ(locator.find(branch, AgentMessage(other)), "b");
    UserMessage missing;
    missing.content = std::string("missing");
    EXPECT_FALSE(locator.find(branch, AgentMessage(missing)).has_value());
}
