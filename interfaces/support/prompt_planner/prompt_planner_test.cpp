#include <gtest/gtest.h>

import std;
import pi.support.prompt_planner;

class PromptPlannerTest : public ::testing::Test {
protected:
    ToolRegistration tool(const std::string& name, const std::string& description = "d") {
        ToolRegistration registration;
        registration.name = name;
        registration.description = description;
        return registration;
    }

    Json systemMessage(const Json& sections, const Json& added = Json(nullptr), const Json& removed = Json(nullptr)) {
        Json message = Json::object({{"role", "system"}, {"content", ""}, {"timestamp", 1}});
        if (!sections.is_null()) {
            message["sections"] = sections;
        }
        if (!added.is_null()) {
            message["toolsAdded"] = added;
        }
        if (!removed.is_null()) {
            message["toolsRemoved"] = removed;
        }
        return message;
    }

    Json view(const Json& messages, const Json& entries = Json::array(), const Json& head = Json(nullptr)) {
        return Json::object({{"head", head}, {"entries", entries}, {"messages", messages}});
    }

    PromptPlanner::Sections sections(std::initializer_list<std::pair<std::string, std::string>> items) {
        return PromptPlanner::Sections(items);
    }

    Json patchOf(const Json& draft) {
        return draft.at("model")[0].at("sections");
    }

    PromptPlanner m_planner;
};

TEST_F(PromptPlannerTest, ReplayAppliesPatchesInPlaceDeletesAndAppendsReadds) {
    Json messages = Json::array({systemMessage(Json::parse(R"({"a":"1","b":"2"})")),
                                 systemMessage(Json::parse(R"({"a":"1b","b":null})")),
                                 systemMessage(Json::parse(R"({"b":"3"})")), Json::object({{"role", "user"}})});
    EXPECT_EQ(m_planner.replaySections(messages), sections({{"a", "1b"}, {"b", "3"}}));
}

TEST_F(PromptPlannerTest, CurrentToolsRemoveThenAddAndReplaceInPlace) {
    Json t1 = Json::parse(R"({"name":"x","description":"1","parameters":{}})");
    Json t2 = Json::parse(R"({"name":"y","description":"1","parameters":{}})");
    Json t1b = Json::parse(R"({"name":"x","description":"2","parameters":{}})");
    Json messages = Json::array({systemMessage(Json(nullptr), Json::array({t1, t2})),
                                 systemMessage(Json(nullptr), Json::array({t1b})),
                                 systemMessage(Json(nullptr), Json(nullptr), Json::array({Json::object({{"name", "y"}})}))});
    std::vector<Json> tools = m_planner.currentTools(messages);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0].at("description"), "2");
}

TEST_F(PromptPlannerTest, RenderWrapsTagsOmitsAndKeepsShownTextWhenASectionFails) {
    std::vector<PromptSection> toRender = {
        PromptSection{"tagged", [](const PromptInput&) -> Result<std::optional<std::string>> { return std::optional<std::string>("body"); }, true},
        PromptSection{"plain", [](const PromptInput&) -> Result<std::optional<std::string>> { return std::optional<std::string>("raw"); }, false},
        PromptSection{"omitted", [](const PromptInput&) -> Result<std::optional<std::string>> { return std::optional<std::string>(); }, true},
        PromptSection{"broken", [](const PromptInput&) -> Result<std::optional<std::string>> { return std::unexpected(Error{"x", "section failed"}); }, true}};
    std::vector<std::string> reports;
    auto desired = m_planner.render(toRender, PromptInput{}, sections({{"broken", "kept"}}),
                                    [&](const Error& error) { reports.push_back(error.message); }, nullptr);
    ASSERT_TRUE(desired.has_value());
    EXPECT_EQ(*desired, sections({{"tagged", "<tagged>\nbody\n</tagged>"}, {"plain", "raw"}, {"broken", "kept"}}));
    EXPECT_EQ(reports, std::vector<std::string>({"section failed"}));
    AbortSignal signal;
    signal.abort();
    EXPECT_FALSE(m_planner.render(toRender, PromptInput{}, {}, [](const Error&) {}, &signal).has_value());
}

TEST_F(PromptPlannerTest, FirstPlanWritesEverythingAndAnUnchangedOneWritesNothing) {
    auto first = m_planner.plan(view(Json::array()), sections({{"env", "E"}}), {tool("read")}, 5);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].at("kind"), "pi.system");
    Json message = first[0].at("model")[0];
    EXPECT_EQ(message.at("sections").dump(), R"({"env":"E"})");
    EXPECT_EQ(message.at("toolsAdded")[0].at("name"), "read");
    EXPECT_FALSE(message.contains("toolsRemoved"));
    EXPECT_EQ(message.at("timestamp"), 5);
    auto again = m_planner.plan(view(Json::array({message})), sections({{"env", "E"}}), {tool("read")}, 6);
    EXPECT_TRUE(again.empty());
}

TEST_F(PromptPlannerTest, MinimalSectionPatchAndRemoval) {
    Json shown = systemMessage(Json::parse(R"({"a":"1","b":"2","c":"3"})"));
    auto drafts = m_planner.plan(view(Json::array({shown})), sections({{"a", "1"}, {"b", "2b"}, {"d", "4"}}), {}, 7);
    ASSERT_EQ(drafts.size(), 1u);
    EXPECT_EQ(patchOf(drafts[0]).dump(), R"({"b":"2b","c":null,"d":"4"})");
}

TEST_F(PromptPlannerTest, ReorderedSectionsRemoveAllThenReaddInOrder) {
    Json shown = systemMessage(Json::parse(R"({"a":"1","b":"2"})"));
    auto drafts = m_planner.plan(view(Json::array({shown})), sections({{"b", "2"}, {"a", "1"}}), {}, 7);
    ASSERT_EQ(drafts.size(), 2u);
    EXPECT_EQ(patchOf(drafts[0]).dump(), R"({"a":null,"b":null})");
    EXPECT_EQ(patchOf(drafts[1]).dump(), R"({"b":"2","a":"1"})");
}

TEST_F(PromptPlannerTest, ToolChangesRideOnTheLastEntryOrTheirOwn) {
    Json shown = systemMessage(Json::parse(R"({"a":"1"})"), Json::array({Json::parse(R"({"name":"x","description":"d","parameters":{}})")}));
    auto onlyTools = m_planner.plan(view(Json::array({shown})), sections({{"a", "1"}}), {tool("x"), tool("y")}, 8);
    ASSERT_EQ(onlyTools.size(), 1u);
    Json message = onlyTools[0].at("model")[0];
    EXPECT_FALSE(message.contains("sections"));
    EXPECT_EQ(message.at("toolsAdded").size(), 1u);
    EXPECT_EQ(message.at("toolsAdded")[0].at("name"), "y");
    auto changed = m_planner.plan(view(Json::array({shown})), sections({{"a", "2"}}), {tool("x", "changed")}, 8);
    ASSERT_EQ(changed.size(), 1u);
    Json both = changed[0].at("model")[0];
    EXPECT_EQ(both.at("sections").dump(), R"({"a":"2"})");
    EXPECT_EQ(both.at("toolsRemoved")[0].at("name"), "x");
    EXPECT_EQ(both.at("toolsAdded")[0].at("description"), "changed");
}

TEST_F(PromptPlannerTest, ReorderedToolsRemoveEveryOfferedToolAndReaddInOrder) {
    Json shown = systemMessage(Json(nullptr), Json::array({Json::parse(R"({"name":"x","description":"d","parameters":{}})"),
                                                           Json::parse(R"({"name":"y","description":"d","parameters":{}})")}));
    auto drafts = m_planner.plan(view(Json::array({shown})), sections({}), {tool("y"), tool("x")}, 9);
    ASSERT_EQ(drafts.size(), 1u);
    Json message = drafts[0].at("model")[0];
    EXPECT_EQ(message.at("toolsRemoved").size(), 2u);
    EXPECT_EQ(message.at("toolsAdded")[0].at("name"), "y");
    EXPECT_EQ(message.at("toolsAdded")[1].at("name"), "x");
}

TEST_F(PromptPlannerTest, AfterAHeadMarkerWithoutSystemEntriesABaselineOmitsEarlierOnes) {
    Json head = Json::object({{"id", 10}, {"kind", "pi.compaction"}, {"head", 7}});
    Json entries = Json::array({head, Json::object({{"id", 8}, {"kind", "pi.system"}}), Json::object({{"id", 9}, {"kind", "pi.user"}})});
    // An older pi.system entry in context but none after the head marker: write a complete baseline even if equal.
    auto drafts = m_planner.plan(view(Json::array(), entries, head), sections({{"a", "1"}}), {tool("x")}, 11);
    ASSERT_EQ(drafts.size(), 1u);
    EXPECT_EQ(drafts[0].at("edits").dump(), R"([{"target":8,"action":"omit"}])");
    EXPECT_EQ(patchOf(drafts[0]).dump(), R"({"a":"1"})");
    EXPECT_EQ(drafts[0].at("model")[0].at("toolsAdded")[0].at("name"), "x");
    // With a later pi.system entry the minimal planning applies again.
    Json withLater = Json::array({head, Json::object({{"id", 12}, {"kind", "pi.system"}})});
    auto minimal = m_planner.plan(view(Json::array({systemMessage(Json::parse(R"({"a":"1"})"))}), withLater, head), sections({{"a", "1"}}), {}, 11);
    EXPECT_TRUE(minimal.empty());
}
