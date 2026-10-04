#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.branch_selection_resolver;

class BranchSelectionResolverTest : public testing::Test {
protected:
    SessionEntry change(const std::string& provider, const std::string& modelId) {
        SessionEntry entry;
        entry.type = "model_change";
        entry.body = Json{{"type", "model_change"}, {"provider", provider}, {"modelId", modelId}};
        return entry;
    }

    SessionEntry response(const std::string& provider, const std::string& model, const std::string& api = "faux", const std::string& role = "assistant") {
        SessionEntry entry;
        entry.type = "message";
        entry.body = Json{{"type", "message"}, {"message", Json{{"role", role}, {"provider", provider}, {"model", model}, {"api", api}}}};
        return entry;
    }

    std::optional<SessionModelRef> resolve(const std::vector<SessionEntry>& branch, bool virtualRegistered = true) {
        return m_resolver.resolve(branch, [virtualRegistered](const std::string& provider, const std::string& id) -> std::optional<Model> {
            Model model;
            model.provider = provider;
            model.id = id;
            if (id == "auto") {
                if (!virtualRegistered) {
                    return std::nullopt;
                }
                model.api = "pi-virtual";
            } else {
                model.api = "faux";
            }
            return model;
        });
    }

    BranchSelectionResolver m_resolver;
};

TEST_F(BranchSelectionResolverTest, AnEmptyBranchSelectsNothing) {
    EXPECT_FALSE(resolve({}).has_value());
    EXPECT_FALSE(resolve({response("a", "m", "faux", "user")}).has_value());
}

TEST_F(BranchSelectionResolverTest, ThePhysicalResponseWinsOverAPhysicalChange) {
    const auto selection = resolve({change("a", "m1"), response("a", "m2")});
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->modelId, "m2");
}

TEST_F(BranchSelectionResolverTest, ALaterChangeWinsOverEarlierResponses) {
    const auto selection = resolve({response("a", "m1"), change("a", "m2")});
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->modelId, "m2");
}

TEST_F(BranchSelectionResolverTest, AVirtualChangeHoldsAcrossTheResponsesItRouted) {
    const auto selection = resolve({change("router", "auto"), response("a", "m1"), response("a", "m2")});
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->provider, "router");
    EXPECT_EQ(selection->modelId, "auto");
}

TEST_F(BranchSelectionResolverTest, AVirtualModelThatIsGoneFallsBackToTheLastResponse) {
    const auto selection = resolve({change("router", "auto"), response("a", "m1")}, false);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->modelId, "m1");
}

TEST_F(BranchSelectionResolverTest, FailedRoutingMessagesAreSkipped) {
    const auto selection = resolve({change("router", "auto"), response("a", "m1"), response("router", "auto", "pi-virtual")});
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->modelId, "auto");
}
