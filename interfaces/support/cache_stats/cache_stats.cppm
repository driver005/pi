module;

#include <cstdint>

export module pi.support.cache_stats;

import std;
export import pi.types.assistant_message;
export import pi.types.cache_miss;
export import pi.types.cache_previous_request;
export import pi.types.cache_scan_result;
export import pi.types.cache_waste_totals;
export import pi.types.model;
export import pi.types.session_entry;
import pi.support.iso_timestamp;
import pi.support.message_codec;

/**
 * Prompt-cache accounting over a session branch: what each turn paid because tokens of the previous prompt were not read
 * from the cache. A compaction or branch summary resets the baseline (the context legitimately changed), a model switch does
 * not (it re-bills the whole prompt), and cache warming requests count as requests. Port of core/cache-stats.ts; misses are
 * keyed by entry id where TypeScript keys them by message object.
 */
export class CacheStats {
public:
    /** Looks a model up by provider and id for its cache-read price; nullopt when unknown. */
    using ModelLookup = std::function<std::optional<Model>(const std::string&, const std::string&)>;

    /** Prompt-cache TTL: idle gaps longer than this are the likely cause of a miss (Anthropic's default is 5 minutes). */
    std::int64_t cacheTtlMs() const {
        return 5 * 60 * 1000;
    }

    CacheWasteTotals computeCacheWaste(const std::vector<SessionEntry>& entries, const ModelLookup& models) const {
        return scan(entries, models, nullptr).totals;
    }

    /** Every counted miss, keyed by the id of the entry whose assistant message paid for it. */
    std::map<std::string, CacheMiss> collectCacheMisses(const std::vector<SessionEntry>& entries, const ModelLookup& models) const {
        std::map<std::string, CacheMiss> misses;
        scan(entries, models, &misses);
        return misses;
    }

    /** The miss of a just-completed message; `entries` must not contain it yet. */
    std::optional<CacheMiss> detectCacheMiss(const std::vector<SessionEntry>& entries, const AssistantMessage& message, const ModelLookup& models) const {
        const CacheScanResult result = scan(entries, models, nullptr);
        return detectMiss(result.previous, message, models);
    }

private:
    CacheScanResult scan(const std::vector<SessionEntry>& entries, const ModelLookup& models, std::map<std::string, CacheMiss>* misses) const {
        CacheScanResult out;
        for (const SessionEntry& entry : entries) {
            if (entry.type == "compaction" || entry.type == "branch_summary") {
                out.previous.reset();
            } else if (entry.type == "usage" && entry.body.value("kind", std::string()) == "cache_warm") {
                warmEntry(entry, out);
            } else if (entry.type == "message" && entry.body.contains("message") && entry.body["message"].value("role", std::string()) == "assistant") {
                messageEntry(entry, models, misses, out);
            }
        }
        return out;
    }

    void warmEntry(const SessionEntry& entry, CacheScanResult& out) const {
        const auto usage = m_codec.usageFromJson(entry.body.contains("usage") ? entry.body["usage"] : Json::object());
        if (!usage) {
            return;
        }
        const std::int64_t promptTokens = usage->input + usage->cacheRead + usage->cacheWrite;
        if (promptTokens > 0) {
            out.previous = CachePreviousRequest{promptTokens, entry.body.value("provider", std::string()) + "/" + entry.body.value("model", std::string()), m_time.parse(entry.timestamp).value_or(0), true};
        }
    }

    void messageEntry(const SessionEntry& entry, const ModelLookup& models, std::map<std::string, CacheMiss>* misses, CacheScanResult& out) const {
        const auto message = assistant(entry.body["message"]);
        if (!message) {
            return;
        }
        if (const auto miss = detectMiss(out.previous, *message, models)) {
            out.totals.missedTokens += miss->missedTokens;
            out.totals.missedCost += miss->missedCost;
            out.totals.missCount += 1;
            if (misses != nullptr) {
                (*misses)[entry.id] = *miss;
            }
        }
        if (const auto next = asPreviousRequest(*message, out.previous && out.previous->reportedCache)) {
            out.previous = next;
        }
    }

    std::optional<AssistantMessage> assistant(const Json& json) const {
        AssistantMessage message;
        message.provider = json.value("provider", std::string());
        message.model = json.value("model", std::string());
        message.timestamp = json.contains("timestamp") && json["timestamp"].is_number() ? json["timestamp"].get<std::int64_t>() : 0;
        const auto usage = m_codec.usageFromJson(json.contains("usage") ? json["usage"] : Json::object());
        if (!usage) {
            return std::nullopt;
        }
        message.usage = *usage;
        return message;
    }

    std::optional<CacheMiss> detectMiss(const std::optional<CachePreviousRequest>& previous, const AssistantMessage& message, const ModelLookup& models) const {
        const Usage& usage = message.usage;
        const std::int64_t promptTokens = usage.input + usage.cacheRead + usage.cacheWrite;
        // A zero-cache turn only counts when cache activity was reported before: on cache-read-only providers that is a total
        // miss, while on providers that never report caching it means nothing.
        if (!previous || promptTokens <= 0 || (usage.cacheRead + usage.cacheWrite == 0 && !previous->reportedCache)) {
            return std::nullopt;
        }
        const std::int64_t missedTokens = std::min(previous->promptTokens, promptTokens) - usage.cacheRead;
        // Per-turn misses at or below this many tokens are cache breakpoint granularity noise.
        if (missedTokens <= 1024) {
            return std::nullopt;
        }
        // Missed tokens can only land in the input or cacheWrite buckets, so the paid rate comes from this message's own cost.
        const std::int64_t paidTokens = usage.input + usage.cacheWrite;
        const double paidPerToken = paidTokens > 0 ? (usage.cost.input + usage.cost.cacheWrite) / static_cast<double>(paidTokens) : 0;
        double readPerToken = 0;
        if (usage.cacheRead > 0) {
            readPerToken = usage.cost.cacheRead / static_cast<double>(usage.cacheRead);
        } else if (const auto model = models(message.provider, message.model)) {
            readPerToken = model->cost.cacheRead / 1'000'000.0;
        }
        CacheMiss miss;
        miss.missedTokens = missedTokens;
        miss.missedCost = static_cast<double>(missedTokens) * std::max(0.0, paidPerToken - readPerToken);
        miss.idleMs = std::max<std::int64_t>(0, message.timestamp - previous->timestamp);
        miss.modelChanged = message.provider + "/" + message.model != previous->modelKey;
        return miss;
    }

    std::optional<CachePreviousRequest> asPreviousRequest(const AssistantMessage& message, bool reportedCache) const {
        const Usage& usage = message.usage;
        const std::int64_t promptTokens = usage.input + usage.cacheRead + usage.cacheWrite;
        if (promptTokens <= 0) {
            return std::nullopt;
        }
        return CachePreviousRequest{promptTokens, message.provider + "/" + message.model, message.timestamp, reportedCache || usage.cacheRead + usage.cacheWrite > 0};
    }

    MessageCodec m_codec;
    IsoTimestamp m_time;
};
