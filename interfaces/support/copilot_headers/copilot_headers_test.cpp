#include <gtest/gtest.h>

import std;
import pi.support.copilot_headers;

TEST(CopilotHeadersTest, InitiatorFollowsLastMessage) {
    CopilotHeaders headers;
    UserMessage user;
    user.content = std::string("hi");
    AssistantMessage assistant;
    EXPECT_EQ(headers.initiator({}), "user");
    EXPECT_EQ(headers.initiator({user}), "user");
    EXPECT_EQ(headers.initiator({user, assistant}), "agent");
}

TEST(CopilotHeadersTest, VisionHeaderWhenImagesPresent) {
    CopilotHeaders headers;
    UserMessage user;
    user.content = std::vector<UserContentBlock>{ImageContent{"d", "image/png"}};
    const auto list = headers.dynamicHeaders({user});
    ASSERT_EQ(list.size(), 3U);
    EXPECT_EQ(list[2].first, "Copilot-Vision-Request");
    UserMessage plain;
    plain.content = std::string("hi");
    EXPECT_EQ(headers.dynamicHeaders({plain}).size(), 2U);
}

TEST(CopilotHeadersTest, ToolResultImagesCount) {
    CopilotHeaders headers;
    ToolResultMessage result;
    result.content.emplace_back(ImageContent{"d", "image/png"});
    EXPECT_TRUE(headers.hasVisionInput({result}));
}
