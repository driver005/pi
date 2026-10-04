#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.hook_bus;

TEST(HookBusTest, HandlersRunInSubscriptionOrderAndResultsAreCollected) {
    HookBus bus;
    std::vector<std::string> order;
    bus.subscribe("e", [&](const std::string& event, const Json&) -> Result<Json> {
        order.push_back("first " + event);
        return Json{{"n", 1}};
    });
    bus.subscribe("e", [&](const std::string&, const Json&) -> Result<Json> {
        order.push_back("second");
        return Json();
    });
    bus.subscribe("e", [&](const std::string&, const Json&) -> Result<Json> {
        order.push_back("third");
        return Json{{"n", 3}};
    });
    bus.subscribe("other", [&](const std::string&, const Json&) -> Result<Json> {
        order.push_back("other");
        return Json();
    });
    const HookOutcome outcome = bus.emit("e", Json{{"x", 1}});
    EXPECT_EQ(order, (std::vector<std::string>{"first e", "second", "third"}));
    ASSERT_EQ(outcome.results.size(), 2U);
    EXPECT_EQ(outcome.results[1]["n"], 3);
    EXPECT_EQ(outcome.payload, (Json{{"x", 1}}));
}

TEST(HookBusTest, ApplyChainsResultsIntoTheNextHandlersPayload) {
    HookBus bus;
    bus.subscribe("e", [](const std::string&, const Json&) -> Result<Json> { return Json{{"add", 1}}; });
    std::vector<int> seen;
    bus.subscribe("e", [&](const std::string&, const Json& payload) -> Result<Json> {
        seen.push_back(payload["total"].get<int>());
        return Json{{"add", 10}};
    });
    const HookOutcome outcome = bus.emit("e", Json{{"total", 0}}, [](Json& payload, const Json& result) {
        payload["total"] = payload["total"].get<int>() + result["add"].get<int>();
    });
    EXPECT_EQ(seen, std::vector<int>{1});
    EXPECT_EQ(outcome.payload["total"], 11);
}

TEST(HookBusTest, FailingHandlersAreReportedAndSkipped) {
    HookBus bus;
    bus.subscribe("e", [](const std::string&, const Json&) -> Result<Json> {
        return std::unexpected(Error{"boom", "it failed"});
    });
    bus.subscribe("e", [](const std::string&, const Json&) -> Result<Json> { return Json{{"ok", true}}; });
    const HookOutcome outcome = bus.emit("e", Json::object());
    ASSERT_EQ(outcome.errors.size(), 1U);
    EXPECT_EQ(outcome.errors[0].message, "it failed");
    EXPECT_EQ(outcome.results.size(), 1U);
}

TEST(HookBusTest, UnsubscribeRemovesHandlers) {
    HookBus bus;
    int calls = 0;
    const auto id = bus.subscribe("e", [&](const std::string&, const Json&) -> Result<Json> {
        ++calls;
        return Json();
    });
    EXPECT_TRUE(bus.hasHandlers("e"));
    EXPECT_FALSE(bus.hasHandlers("missing"));
    bus.unsubscribe(id);
    EXPECT_FALSE(bus.hasHandlers("e"));
    bus.emit("e", Json::object());
    EXPECT_EQ(calls, 0);
}
