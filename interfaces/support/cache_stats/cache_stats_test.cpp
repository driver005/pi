#include <gtest/gtest.h>

import std;
import pi.support.cache_stats;

class CacheStatsTest : public testing::Test {
protected:
    SessionEntry assistant(const std::string& id, std::int64_t input, std::int64_t cacheRead, std::int64_t cacheWrite, std::int64_t timestamp, const std::string& model = "m") {
        SessionEntry entry;
        entry.type = "message";
        entry.id = id;
        entry.body = Json{{"type", "message"}, {"id", id}, {"message", Json{{"role", "assistant"}, {"provider", "p"}, {"model", model}, {"timestamp", timestamp}, {"usage", usage(input, cacheRead, cacheWrite)}}}};
        return entry;
    }

    Json usage(std::int64_t input, std::int64_t cacheRead, std::int64_t cacheWrite) {
        // $3/M input, $3.75/M cache write, $0.3/M cache read.
        const double inputCost = static_cast<double>(input) * 3.0 / 1e6;
        const double writeCost = static_cast<double>(cacheWrite) * 3.75 / 1e6;
        const double readCost = static_cast<double>(cacheRead) * 0.3 / 1e6;
        return Json{{"input", input}, {"output", 10}, {"cacheRead", cacheRead}, {"cacheWrite", cacheWrite}, {"totalTokens", input + cacheRead + cacheWrite + 10},
                    {"cost", Json{{"input", inputCost}, {"output", 0}, {"cacheRead", readCost}, {"cacheWrite", writeCost}, {"total", inputCost + writeCost + readCost}}}};
    }

    CacheStats::ModelLookup models() {
        return [](const std::string&, const std::string&) -> std::optional<Model> {
            Model model;
            model.cost.cacheRead = 0.3;
            return model;
        };
    }

    AssistantMessage message(std::int64_t input, std::int64_t cacheRead, std::int64_t cacheWrite, std::int64_t timestamp, const std::string& model = "m") {
        const Json json = usage(input, cacheRead, cacheWrite);
        AssistantMessage out;
        out.provider = "p";
        out.model = model;
        out.timestamp = timestamp;
        out.usage.input = input;
        out.usage.cacheRead = cacheRead;
        out.usage.cacheWrite = cacheWrite;
        out.usage.cost.input = json["cost"]["input"].get<double>();
        out.usage.cost.cacheRead = json["cost"]["cacheRead"].get<double>();
        out.usage.cost.cacheWrite = json["cost"]["cacheWrite"].get<double>();
        return out;
    }

    CacheStats m_stats;
};

TEST_F(CacheStatsTest, ACacheHitCountsNothing) {
    const std::vector<SessionEntry> entries{assistant("a", 100, 0, 20000, 1000), assistant("b", 100, 20000, 500, 2000)};
    const CacheWasteTotals totals = m_stats.computeCacheWaste(entries, models());
    EXPECT_EQ(totals.missCount, 0);
}

TEST_F(CacheStatsTest, ATurnThatRebillsThePreviousPromptIsAMiss) {
    const std::vector<SessionEntry> entries{assistant("a", 100, 0, 20000, 1000), assistant("b", 20100, 0, 0, 400000)};
    const CacheWasteTotals totals = m_stats.computeCacheWaste(entries, models());
    EXPECT_EQ(totals.missCount, 1);
    EXPECT_EQ(totals.missedTokens, 20100);
    EXPECT_NEAR(totals.missedCost, 20100 * (3.0 - 0.3) / 1e6, 1e-9);
    const auto misses = m_stats.collectCacheMisses(entries, models());
    ASSERT_EQ(misses.count("b"), 1U);
    EXPECT_EQ(misses.at("b").idleMs, 399000);
    EXPECT_FALSE(misses.at("b").modelChanged);
}

TEST_F(CacheStatsTest, SmallMissesBelowTheNoiseFloorAreIgnored) {
    const std::vector<SessionEntry> entries{assistant("a", 100, 0, 20000, 1000), assistant("b", 100, 19500, 1000, 2000)};
    EXPECT_EQ(m_stats.computeCacheWaste(entries, models()).missCount, 0);
}

TEST_F(CacheStatsTest, ProvidersThatNeverReportCachingAreNotCounted) {
    const std::vector<SessionEntry> entries{assistant("a", 20000, 0, 0, 1000), assistant("b", 20100, 0, 0, 2000)};
    EXPECT_EQ(m_stats.computeCacheWaste(entries, models()).missCount, 0);
}

TEST_F(CacheStatsTest, CompactionResetsTheBaselineButModelSwitchesCount) {
    SessionEntry compaction;
    compaction.type = "compaction";
    compaction.id = "c";
    const std::vector<SessionEntry> reset{assistant("a", 100, 0, 20000, 1000), compaction, assistant("b", 15000, 0, 0, 2000)};
    EXPECT_EQ(m_stats.computeCacheWaste(reset, models()).missCount, 0);
    const std::vector<SessionEntry> switched{assistant("a", 100, 0, 20000, 1000), assistant("b", 20100, 0, 0, 2000, "other")};
    const auto misses = m_stats.collectCacheMisses(switched, models());
    ASSERT_EQ(misses.count("b"), 1U);
    EXPECT_TRUE(misses.at("b").modelChanged);
}

TEST_F(CacheStatsTest, CacheWarmingUsageEntriesRefreshTheBaseline) {
    SessionEntry warm;
    warm.type = "usage";
    warm.id = "w";
    warm.timestamp = "1970-01-01T00:00:03.000Z";
    warm.body = Json{{"type", "usage"}, {"kind", "cache_warm"}, {"provider", "p"}, {"model", "m"}, {"usage", usage(0, 20000, 0)}};
    const std::vector<SessionEntry> entries{assistant("a", 100, 0, 20000, 1000), warm};
    const auto miss = m_stats.detectCacheMiss(entries, message(20100, 0, 0, 5000), models());
    ASSERT_TRUE(miss.has_value());
    EXPECT_EQ(miss->idleMs, 2000);
    EXPECT_FALSE(m_stats.detectCacheMiss(entries, message(100, 20000, 0, 5000), models()).has_value());
}
