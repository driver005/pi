#include <gtest/gtest.h>

import std;
import pi.support.event_stream;

TEST(EventStreamTest, DeliversEventsInOrderAndExtractsResult) {
    EventStream<int, std::string> stream([](const int& e) { return e < 0; },
                                         [](const int& e) { return std::to_string(e); });
    std::thread producer([&] {
        stream.push(1);
        stream.push(2);
        stream.push(-7);
        stream.push(99);
    });
    std::vector<int> seen;
    while (auto event = stream.next()) {
        seen.push_back(*event);
    }
    producer.join();
    EXPECT_EQ(seen, (std::vector<int>{1, 2, -7}));
    EXPECT_EQ(stream.result(), std::optional<std::string>("-7"));
}

TEST(EventStreamTest, EndWithoutTerminalEvent) {
    EventStream<int, int> stream([](const int&) { return false; }, [](const int& e) { return e; });
    stream.push(3);
    stream.end(5);
    EXPECT_EQ(stream.next(), std::optional<int>(3));
    EXPECT_EQ(stream.next(), std::nullopt);
    EXPECT_EQ(stream.result(), std::optional<int>(5));
}

TEST(EventStreamTest, EndWithoutResultYieldsNullopt) {
    EventStream<int, int> stream([](const int&) { return false; }, [](const int& e) { return e; });
    stream.end();
    EXPECT_EQ(stream.next(), std::nullopt);
    EXPECT_EQ(stream.result(), std::nullopt);
}
