#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.chord.replicated_state;
import pi.support.delta_applier;

class ReplicatedStateTest : public testing::Test {
protected:
    struct Seen {
        Json ops;
        std::int64_t sequence = 0;
    };

    std::vector<Seen> watch(ReplicatedState& state) {
        m_seen.clear();
        state.subscribe([this](const Json& ops, std::int64_t sequence, const ServiceContext&) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_seen.push_back(Seen{ops, sequence});
        });
        return m_seen;
    }

    ServiceContext m_context;
    std::mutex m_mutex;
    std::vector<Seen> m_seen;
};

TEST_F(ReplicatedStateTest, StartsAtSequenceZero) {
    ReplicatedState state(Json{{"count", 1}});
    const auto snapshot = state.snapshot();
    EXPECT_EQ(snapshot.value, (Json{{"count", 1}}));
    EXPECT_EQ(snapshot.sequence, 0);
}

TEST_F(ReplicatedStateTest, ChangePublishesTheDiff) {
    ReplicatedState state(Json{{"count", 1}, {"nested", Json{{"text", "a"}}}, {"values", Json::array({1})}});
    watch(state);
    ASSERT_TRUE(state.change(m_context, [](Json& draft) {
        draft["count"] = 2;
        draft["nested"]["text"] = "ab";
        draft["values"].push_back(2);
    }).has_value());
    ASSERT_EQ(m_seen.size(), 1U);
    EXPECT_EQ(m_seen[0].sequence, 1);
    EXPECT_EQ(m_seen[0].ops, Json::parse(R"([["s",["count"],2],["a",["nested","text"],"b"],["p",["values"],1,0,[2]]])"));
    EXPECT_EQ(state.snapshot().sequence, 1);
    EXPECT_EQ(state.value()["count"], 2);
}

TEST_F(ReplicatedStateTest, UnchangedValuesPublishNothing) {
    ReplicatedState state(Json{{"a", 1}});
    watch(state);
    ASSERT_TRUE(state.change(m_context, [](Json&) {}).has_value());
    ASSERT_TRUE(state.change(m_context, [](Json& draft) { draft["a"] = 1; }).has_value());
    ASSERT_TRUE(state.replace(m_context, Json{{"a", 1}}).has_value());
    EXPECT_TRUE(m_seen.empty());
    EXPECT_EQ(state.snapshot().sequence, 0);
}

TEST_F(ReplicatedStateTest, ReplacePublishesARootReplacement) {
    ReplicatedState state(Json{{"a", 1}});
    watch(state);
    ASSERT_TRUE(state.replace(m_context, Json{{"b", 2}}).has_value());
    ASSERT_EQ(m_seen.size(), 1U);
    EXPECT_EQ(m_seen[0].ops, Json::parse(R"([["r",{"b":2}]])"));
    EXPECT_EQ(state.value(), (Json{{"b", 2}}));
}

TEST_F(ReplicatedStateTest, OpsReplayToTheSameValue) {
    ReplicatedState state(Json{{"rows", Json::array()}});
    Json replica = state.value();
    const DeltaApplier applier;
    state.subscribe([&](const Json& ops, std::int64_t, const ServiceContext&) { replica = *applier.apply(replica, ops); });
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(state.change(m_context, [i](Json& draft) { draft["rows"].push_back(Json{{"id", i}}); }).has_value());
    }
    EXPECT_EQ(replica, state.value());
}

TEST_F(ReplicatedStateTest, ReentrantChangesFromListenersAreDeliveredInOrder) {
    ReplicatedState state(Json{{"n", 0}});
    std::vector<std::int64_t> order;
    state.subscribe([&](const Json&, std::int64_t sequence, const ServiceContext&) {
        order.push_back(sequence);
        if (sequence == 1) {
            ASSERT_TRUE(state.change(m_context, [](Json& draft) { draft["n"] = 2; }).has_value());
            order.push_back(-1);
        }
    });
    ASSERT_TRUE(state.change(m_context, [](Json& draft) { draft["n"] = 1; }).has_value());
    EXPECT_EQ(order, (std::vector<std::int64_t>{1, -1, 2}));
}

TEST_F(ReplicatedStateTest, ChangesInsideAChangeCallbackAreRejected) {
    ReplicatedState state(Json{{"n", 0}});
    Result<void> inner;
    ASSERT_TRUE(state.change(m_context, [&](Json& draft) {
        inner = state.change(m_context, [](Json&) {});
        draft["n"] = 1;
    }).has_value());
    ASSERT_FALSE(inner.has_value());
    EXPECT_NE(inner.error().message.find("reentrantly"), std::string::npos);
    EXPECT_EQ(state.value()["n"], 1);
}

TEST_F(ReplicatedStateTest, UnsubscribedListenersStopHearing) {
    ReplicatedState state(Json{{"n", 0}});
    int calls = 0;
    const auto id = state.subscribe([&](const Json&, std::int64_t, const ServiceContext&) { ++calls; });
    ASSERT_TRUE(state.change(m_context, [](Json& draft) { draft["n"] = 1; }).has_value());
    state.unsubscribe(id);
    ASSERT_TRUE(state.change(m_context, [](Json& draft) { draft["n"] = 2; }).has_value());
    EXPECT_EQ(calls, 1);
}

TEST_F(ReplicatedStateTest, ConcurrentChangesAreSerializedAndDeliveredInSequence) {
    ReplicatedState state(Json{{"count", 0}});
    std::vector<std::int64_t> sequences;
    std::mutex mutex;
    state.subscribe([&](const Json&, std::int64_t sequence, const ServiceContext&) {
        const std::lock_guard<std::mutex> lock(mutex);
        sequences.push_back(sequence);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 50; ++i) {
                state.change(m_context, [](Json& draft) { draft["count"] = draft["count"].get<int>() + 1; });
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(state.value()["count"], 200);
    ASSERT_EQ(sequences.size(), 200U);
    EXPECT_TRUE(std::is_sorted(sequences.begin(), sequences.end()));
    EXPECT_EQ(sequences.back(), 200);
}
