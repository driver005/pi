#include <gtest/gtest.h>

import std;
import pi.support.event_bus;

TEST(EventBusTest, HandlersRunInSubscriptionOrderForTheirChannelOnly) {
    EventBus bus;
    std::vector<std::string> seen;
    bus.on("a", [&](const std::string& channel, const Json& data) { seen.push_back("1:" + channel + data.dump()); });
    bus.on("a", [&](const std::string&, const Json&) { seen.push_back("2"); });
    bus.on("b", [&](const std::string&, const Json&) { seen.push_back("b"); });
    bus.emit("a", Json{{"x", 1}});
    EXPECT_EQ(seen, (std::vector<std::string>{"1:a{\"x\":1}", "2"}));
    bus.emit("none", Json());
    EXPECT_EQ(seen.size(), 2U);
}

TEST(EventBusTest, OffAndClearRemoveSubscriptions) {
    EventBus bus;
    int calls = 0;
    const auto first = bus.on("a", [&](const std::string&, const Json&) { ++calls; });
    bus.on("a", [&](const std::string&, const Json&) { calls += 10; });
    bus.off(first);
    bus.emit("a", Json());
    EXPECT_EQ(calls, 10);
    bus.clear();
    bus.emit("a", Json());
    EXPECT_EQ(calls, 10);
}

TEST(EventBusTest, AHandlerMaySubscribeWhileRunning) {
    EventBus bus;
    int late = 0;
    bus.on("a", [&](const std::string&, const Json&) { bus.on("a", [&](const std::string&, const Json&) { ++late; }); });
    bus.emit("a", Json());
    EXPECT_EQ(late, 0);
    bus.emit("a", Json());
    EXPECT_EQ(late, 1);
}
