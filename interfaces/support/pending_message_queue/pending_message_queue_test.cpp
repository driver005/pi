#include "interfaces/support/pending_message_queue/pending_message_queue.h"

#include <gtest/gtest.h>

class PendingMessageQueueTest : public testing::Test {
protected:
    AgentMessage text(const std::string& value) {
        UserMessage message;
        message.content = value;
        return message;
    }

    std::string textOf(const AgentMessage& message) {
        return std::get<std::string>(std::get<UserMessage>(message).content);
    }
};

TEST_F(PendingMessageQueueTest, OneAtATimeDrainsOldestOnly) {
    PendingMessageQueue queue(QueueMode::OneAtATime);
    queue.enqueue(text("a"));
    queue.enqueue(text("b"));
    EXPECT_EQ(queue.peek().size(), 1U);
    const auto first = queue.drain();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(textOf(first[0]), "a");
    EXPECT_TRUE(queue.hasItems());
    EXPECT_EQ(textOf(queue.drain()[0]), "b");
    EXPECT_FALSE(queue.hasItems());
    EXPECT_TRUE(queue.drain().empty());
}

TEST_F(PendingMessageQueueTest, AllModeDrainsEverything) {
    PendingMessageQueue queue(QueueMode::All);
    queue.enqueue(text("a"));
    queue.enqueue(text("b"));
    EXPECT_EQ(queue.drain().size(), 2U);
    EXPECT_FALSE(queue.hasItems());
}

TEST_F(PendingMessageQueueTest, ClearAndModeSwitch) {
    PendingMessageQueue queue(QueueMode::OneAtATime);
    queue.enqueue(text("a"));
    queue.enqueue(text("b"));
    queue.setMode(QueueMode::All);
    EXPECT_EQ(queue.mode(), QueueMode::All);
    EXPECT_EQ(queue.peek().size(), 2U);
    queue.clear();
    EXPECT_FALSE(queue.hasItems());
}
