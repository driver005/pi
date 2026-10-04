#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.cache_warmer;
import pi.testing.fake_environment;
import pi.testing.fake_model_runtime;
import pi.testing.session_harness;

/** A sleeper that waits until the test releases it (or the signal aborts), so refreshes fire one at a time. */
class GatedSleeper : public ISleeper {
public:
    bool sleep(std::chrono::milliseconds duration, const std::shared_ptr<AbortSignal>& signal) override {
        const std::uint64_t link = signal->onAbort([this] {
            const std::lock_guard<std::mutex> guard(m_mutex);
            m_changed.notify_all();
        });
        std::unique_lock<std::mutex> lock(m_mutex);
        m_delays.push_back(duration.count());
        ++m_waiting;
        m_changed.notify_all();
        m_changed.wait(lock, [&] { return m_permits > 0 || signal->aborted(); });
        --m_waiting;
        const bool released = m_permits > 0 && !signal->aborted();
        if (released) {
            --m_permits;
        }
        lock.unlock();
        signal->removeListener(link);
        return released;
    }

    /** Waits until a sleep is pending, then lets it end. */
    void release() {
        waitForSleep();
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_permits;
        m_changed.notify_all();
    }

    void waitForSleep() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_waiting > m_permits; });
    }

    std::vector<std::int64_t> delays() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_delays;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::int64_t> m_delays;
    int m_waiting = 0;
    int m_permits = 0;
};

class CacheWarmerTest : public testing::Test {
protected:
    CacheWarmerTest() : m_harness("/work") {
        m_model.id = "m";
        m_model.provider = "p";
        m_model.api = "faux";
        m_model.cost.input = 3;
        m_model.cost.output = 15;
        m_model.cost.cacheRead = 0.3;
        m_model.cost.cacheWrite = 3.75;
        m_model.promptCache = Json{{"short", 300}, {"long", 3600}};
        m_runtime.addModel(m_model);
        m_runtime.setAuthenticated("p", true);
        m_runtime.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            {
                const std::lock_guard<std::mutex> lock(m_dataMutex);
                m_requestLog.push_back(options);
            }
            return m_harness.provider().stream(model, context, options);
        });
        seedPrompt(100000);
    }

    ~CacheWarmerTest() override {
        m_warmer.reset();
    }

    void seedPrompt(std::int64_t cacheWrite) {
        AssistantMessage reply = m_harness.provider().textResponse("earlier");
        reply.provider = "p";
        reply.model = "m";
        reply.usage.input = 100;
        reply.usage.cacheWrite = cacheWrite;
        m_harness.session().appendMessage(reply);
    }

    void makeWarmer(const std::string& mode, CacheWarmer::Decide decide = {}) {
        m_mode = mode;
        m_warmer = std::make_unique<CacheWarmer>(m_runtime, m_harness.session(), m_sleeper, m_harness.clock(), m_environment, [this] { return m_mode; }, std::move(decide));
        m_warmer->setOnWarmed([this](const SessionEntry& entry) {
            const std::lock_guard<std::mutex> lock(m_dataMutex);
            m_warmedLog.push_back(entry);
        });
    }

    CacheWarmRequest request() {
        CacheWarmRequest out;
        out.model = m_model;
        return out;
    }

    void respond(std::int64_t cacheRead) {
        AssistantMessage message = m_harness.provider().textResponse("w");
        message.provider = "p";
        message.model = "m";
        message.usage.cacheRead = cacheRead;
        message.usage.output = 1;
        m_harness.provider().enqueue(message);
    }

    std::vector<SessionEntry> usageEntries() {
        std::vector<SessionEntry> out;
        for (const SessionEntry& entry : m_harness.session().entries()) {
            if (entry.type == "usage") {
                out.push_back(entry);
            }
        }
        return out;
    }

    std::vector<StreamOptions> requests() {
        const std::lock_guard<std::mutex> lock(m_dataMutex);
        return m_requestLog;
    }

    void waitForWarmed(std::size_t count) {
        for (;;) {
            {
                const std::lock_guard<std::mutex> lock(m_dataMutex);
                if (m_warmedLog.size() >= count) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::string m_mode;
    Model m_model;
    SessionHarness m_harness;
    GatedSleeper m_sleeper;
    FakeEnvironment m_environment;
    FakeModelRuntime m_runtime;
    std::mutex m_dataMutex;
    std::vector<StreamOptions> m_requestLog;
    std::vector<SessionEntry> m_warmedLog;
    std::unique_ptr<CacheWarmer> m_warmer;
};

TEST_F(CacheWarmerTest, SchedulesARefreshAtNinetyPercentOfTheTtlWithTenSecondsOfMargin) {
    makeWarmer("streaming");
    EXPECT_EQ(m_warmer->warmingDelayMs(300000), 270000);
    EXPECT_EQ(m_warmer->warmingDelayMs(60000), 50000);
    EXPECT_FALSE(m_warmer->warmingDelayMs(10000).has_value());
    m_warmer->start(request(), [] { return true; });
    m_sleeper.waitForSleep();
    EXPECT_EQ(m_sleeper.delays(), std::vector<std::int64_t>{270000});
    const CacheWarmingStatus status = m_warmer->status();
    EXPECT_EQ(status.state, "scheduled");
    ASSERT_TRUE(status.decision.has_value());
    EXPECT_TRUE(status.decision->economicsAvailable);
    EXPECT_EQ(status.decision->action, "warm");
    EXPECT_EQ(status.nextWarmAt, m_harness.clock().nowMs() + 270000);
}

TEST_F(CacheWarmerTest, ARefreshSendsAOneTokenRequestRecordsItsUsageAndSchedulesAgain) {
    makeWarmer("streaming");
    respond(100100);
    m_warmer->start(request(), [] { return true; });
    m_sleeper.release();
    waitForWarmed(1);
    const auto sent = requests();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].maxTokens, 1);
    EXPECT_EQ(sent[0].maxRetries, 0);
    const auto entries = usageEntries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].body["kind"], "cache_warm");
    EXPECT_EQ(entries[0].body["provider"], "p");
    EXPECT_TRUE(entries[0].body["usage"].contains("totalTokens")) << "the usage the provider reported is stored";
    m_sleeper.waitForSleep();
    EXPECT_EQ(m_warmer->status().state, "scheduled");
}

TEST_F(CacheWarmerTest, StartRefusesWhatCannotBeWarmed) {
    makeWarmer("off");
    m_warmer->start(request(), [] { return true; });
    EXPECT_EQ(m_warmer->status().reason, "cache warming disabled");

    makeWarmer("streaming");
    CacheWarmRequest none = request();
    none.options.cacheRetention = "none";
    m_warmer->start(none, [] { return true; });
    EXPECT_EQ(m_warmer->status().reason, "request disabled prompt caching");

    CacheWarmRequest noTier = request();
    noTier.model.promptCache = Json();
    m_warmer->start(noTier, [] { return true; });
    EXPECT_EQ(m_warmer->status().reason, "cache lifetime unavailable");

    CacheWarmRequest shortLived = request();
    shortLived.model.promptCache = Json{{"short", 5}};
    m_warmer->start(shortLived, [] { return true; });
    EXPECT_EQ(m_warmer->status().reason, "cache lifetime unavailable");

    CacheWarmRequest budgeted = request();
    budgeted.model.api = "anthropic-messages";
    budgeted.options.reasoning = ThinkingLevel::High;
    m_warmer->start(budgeted, [] { return true; });
    EXPECT_EQ(m_warmer->status().reason, "request cannot be replayed safely");
    budgeted.model.compat = Json{{"forceAdaptiveThinking", true}};
    m_warmer->start(budgeted, [] { return true; });
    EXPECT_EQ(m_warmer->status().state, "scheduled");
}

TEST_F(CacheWarmerTest, TheLongRetentionEnvironmentPicksTheLongTier) {
    makeWarmer("streaming");
    m_environment.set("PI_CACHE_RETENTION", "long");
    EXPECT_EQ(m_warmer->promptCacheTtlMs(m_model, StreamOptions{}), 3600000);
    StreamOptions explicitShort;
    explicitShort.cacheRetention = "short";
    EXPECT_EQ(m_warmer->promptCacheTtlMs(m_model, explicitShort), 300000);
    StreamOptions fromOptions;
    fromOptions.env["PI_CACHE_RETENTION"] = "short";
    EXPECT_EQ(m_warmer->promptCacheTtlMs(m_model, fromOptions), 300000);
}

TEST_F(CacheWarmerTest, ARefreshThatSavesTooLittleStopsTheWarming) {
    makeWarmer("idle");
    m_warmer->start(request(), [] { return true; });
    m_warmer->onAgentSettled();
    m_sleeper.release();
    while (m_warmer->status().state != "inactive") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const CacheWarmingStatus status = m_warmer->status();
    EXPECT_EQ(status.reason, "expected savings below threshold");
    ASSERT_TRUE(status.decision.has_value());
    EXPECT_EQ(status.decision->phase, "idle");
    EXPECT_NEAR(status.decision->continuationProbability, 0.15, 1e-9);
    EXPECT_TRUE(requests().empty());
}

TEST_F(CacheWarmerTest, ThePluginDecisionOverridesPisChoice) {
    std::vector<CacheWarmingDecision> seen;
    makeWarmer("idle", [&seen](const CacheWarmingDecision& decision) {
        seen.push_back(decision);
        return std::string("warm");
    });
    respond(100100);
    m_warmer->start(request(), [] { return true; });
    m_warmer->onAgentSettled();
    m_sleeper.release();
    waitForWarmed(1);
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].action, "stop");
    const auto entries = usageEntries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].body["note"], "extension override");

    makeWarmer("streaming", [](const CacheWarmingDecision&) { return std::string("stop"); });
    m_warmer->start(request(), [] { return true; });
    m_sleeper.release();
    while (m_warmer->status().state != "inactive") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(m_warmer->status().reason, "stopped by extension");
    EXPECT_TRUE(m_warmer->status().extensionOverride);
}

TEST_F(CacheWarmerTest, StreamingModeStopsWhenTheRunSettlesAndIdleModeKeepsGoing) {
    makeWarmer("streaming");
    m_warmer->start(request(), [] { return true; });
    m_warmer->onAgentSettled();
    EXPECT_EQ(m_warmer->status().reason, "agent run settled");
    makeWarmer("idle");
    m_warmer->start(request(), [] { return true; });
    m_warmer->onAgentSettled();
    EXPECT_EQ(m_warmer->status().state, "scheduled");
    m_mode = "off";
    m_warmer->onModeChanged();
    EXPECT_EQ(m_warmer->status().reason, "cache warming disabled");
}

TEST_F(CacheWarmerTest, AChangedConversationEndsTheRunAtTheNextRefresh) {
    makeWarmer("streaming");
    std::atomic<bool> current = true;
    m_warmer->start(request(), [&current] { return current.load(); });
    m_sleeper.waitForSleep();
    current = false;
    EXPECT_EQ(m_warmer->status().reason, "conversation context changed");
    m_sleeper.release();
    m_warmer.reset();
    EXPECT_TRUE(requests().empty());
}

TEST_F(CacheWarmerTest, ALateRefreshMissesItsDeadline) {
    makeWarmer("streaming");
    m_warmer->start(request(), [] { return true; });
    m_sleeper.waitForSleep();
    m_harness.clock().advance(270000 + 15000 + 1);
    m_sleeper.release();
    while (m_warmer->status().state != "inactive") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(m_warmer->status().reason, "cache refresh deadline missed");
}

TEST_F(CacheWarmerTest, TheOneHourSafetyWindowEndsStreamingWarming) {
    makeWarmer("streaming");
    m_runtime.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        m_harness.clock().advance(60 * 60'000);
        return m_harness.provider().stream(model, context, options);
    });
    respond(100100);
    m_warmer->start(request(), [] { return true; });
    m_sleeper.release();
    waitForWarmed(1);
    while (m_warmer->status().state != "inactive") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(m_warmer->status().reason, "one-hour safety limit reached");
}
