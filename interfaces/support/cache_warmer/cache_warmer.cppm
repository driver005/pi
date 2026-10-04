module;

#include <cstdint>

export module pi.support.cache_warmer;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_environment;
export import pi.platform.i_sleeper;
export import pi.provider.i_model_runtime;
export import pi.session.i_session_manager;
export import pi.types.cache_warm_request;
export import pi.types.cache_warm_run;
export import pi.types.cache_warming_decision;
export import pi.types.cache_warming_status;
import pi.support.cost_calculator;
import pi.support.thinking_level_resolver;

/**
 * Keeps one prompt-cache entry alive by sending its request again, with a one-token output cap, before the entry expires.
 * start() replaces the previous run; the safety windows (one hour while streaming, 30 minutes idle) are fixed from the first
 * request and refreshes never extend them. Each refresh is priced first: it is sent when it is expected to save at least
 * $0.05, or when the `decide` callback (the `cache_warming_decision` plugin event) says "warm". The warm request is billed, so
 * its usage is appended to the session as a `cache_warm` usage entry. Port of core/cache-warmer.ts; a timer thread per
 * scheduled refresh stands where the JavaScript timer was.
 */
export class CacheWarmer {
public:
    /** Returns "warm" or "stop" for the decision pi made. */
    using Decide = std::function<std::string(const CacheWarmingDecision&)>;
    using Warmed = std::function<void(const SessionEntry&)>;
    /** "off", "streaming" or "idle" (the cacheWarming setting). */
    using Mode = std::function<std::string()>;

    CacheWarmer(IModelRuntime& models, ISessionManager& session, ISleeper& sleeper, const IClock& clock, const IEnvironment& environment, Mode mode, Decide decide = {})
        : m_models(models),
          m_session(session),
          m_sleeper(sleeper),
          m_clock(clock),
          m_environment(environment),
          m_mode(std::move(mode)),
          m_decide(std::move(decide)) {
        m_inactive.reason = "waiting for first request";
    }

    ~CacheWarmer() {
        cancel();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_threads == 0; });
    }

    CacheWarmer(const CacheWarmer&) = delete;
    CacheWarmer& operator=(const CacheWarmer&) = delete;

    /** Called with the usage entry after each successful refresh. */
    void setOnWarmed(Warmed warmed) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_onWarmed = std::move(warmed);
    }

    /** Refresh at 90% of the TTL while keeping at least ten seconds of margin; nullopt when the TTL is too short to warm. */
    std::optional<std::int64_t> warmingDelayMs(std::int64_t ttlMs) const {
        if (ttlMs <= 10'000) {
            return std::nullopt;
        }
        return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::floor(std::min(static_cast<double>(ttlMs) * 0.9, static_cast<double>(ttlMs - 10'000)))));
    }

    /** The lifetime of the cache entry a request writes, from the model's `promptCache` tier; nullopt when there is none. */
    std::optional<std::int64_t> promptCacheTtlMs(const Model& model, const StreamOptions& options) const {
        std::string retention = "short";
        if (options.cacheRetention) {
            retention = *options.cacheRetention;
        } else {
            const auto fromOptions = options.env.find("PI_CACHE_RETENTION");
            const std::optional<std::string> value = fromOptions != options.env.end() ? std::optional<std::string>(fromOptions->second) : m_environment.get("PI_CACHE_RETENTION");
            retention = value && *value == "long" ? "long" : "short";
        }
        if (retention == "none" || !model.promptCache.is_object() || !model.promptCache.contains(retention) || !model.promptCache[retention].is_number()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(model.promptCache[retention].get<double>() * 1000);
    }

    /**
     * Whether replaying the request with a one-token output cap leaves its cache entry untouched. Anthropic's budget-based
     * thinking derives `budget_tokens` from `max_tokens`, so the replay would change the budget the cache is keyed on.
     */
    bool isReplayable(const Model& model, const StreamOptions& options) const {
        if (options.reasoning == ThinkingLevel::Off || model.api != "anthropic-messages") {
            return true;
        }
        return model.compat.is_object() && model.compat.contains("forceAdaptiveThinking") && model.compat["forceAdaptiveThinking"] == true;
    }

    CacheWarmingStatus status() const {
        std::shared_ptr<CacheWarmRun> run;
        CacheWarmingStatus inactive;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_mode() == "off") {
                CacheWarmingStatus off;
                off.reason = "cache warming disabled";
                return off;
            }
            run = m_run;
            inactive = m_inactive;
        }
        if (!run) {
            return inactive;
        }
        CacheWarmingStatus out;
        if (run->isCurrent && !run->isCurrent()) {
            out.reason = "conversation context changed";
            return out;
        }
        const CacheWarmingDecision decision = evaluate(*run);
        const std::lock_guard<std::mutex> lock(m_mutex);
        const bool refreshing = !run->timerArmed;
        if (!decision.economicsAvailable && !refreshing) {
            out.reason = "cache economics unavailable";
            return out;
        }
        out.state = refreshing ? "refreshing" : "scheduled";
        out.nextWarmAt = run->nextWarmAt;
        out.decision = decision;
        out.extensionOverride = run->extensionOverride;
        return out;
    }

    /** Keeps the cache entry written by `request` warm while `isCurrent` holds. */
    void start(CacheWarmRequest request, std::function<bool()> isCurrent) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        clearRun();
        if (m_mode() == "off") {
            stop("cache warming disabled");
            return;
        }
        if (!isReplayable(request.model, request.options)) {
            stop("request cannot be replayed safely");
            return;
        }
        const auto ttl = promptCacheTtlMs(request.model, request.options);
        if (!ttl) {
            stop(request.options.cacheRetention == "none" ? "request disabled prompt caching" : "cache lifetime unavailable");
            return;
        }
        const auto delay = warmingDelayMs(*ttl);
        if (!delay) {
            stop("cache lifetime unavailable");
            return;
        }
        auto run = std::make_shared<CacheWarmRun>();
        run->request = std::move(request);
        run->isCurrent = std::move(isCurrent);
        run->ttlMs = *ttl;
        run->delayMs = *delay;
        run->startedAt = m_clock.nowMs();
        m_run = run;
        schedule(run);
    }

    void onAgentSettled() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_run) {
            return;
        }
        if (m_mode() == "streaming") {
            stop("agent run settled");
            return;
        }
        m_run->phase = "idle";
        const std::int64_t deadline = m_run->startedAt + kMaxIdleAgeMs;
        if (m_run->nextWarmAt > deadline || m_clock.nowMs() >= deadline) {
            stop("30-minute idle safety limit reached");
        }
    }

    /** Reconciles an active run after the cacheWarming setting changed. */
    void onModeChanged() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_run) {
            return;
        }
        if (const auto reason = modeStopReason(*m_run)) {
            stop(*reason);
        }
    }

    void cancel() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        stop("inactive");
    }

private:
    /** Streaming warming never continues past this long after the real request that started it (one hour). */
    static constexpr std::int64_t kMaxAgeMs = 60 * 60'000;
    /** Idle warming uses a shorter horizon because continuation estimates get less reliable with age. */
    static constexpr std::int64_t kMaxIdleAgeMs = 30 * 60'000;

    /** Ends the run (waking its timer) and remembers why; m_mutex is held. */
    void stop(const std::string& reason, const std::optional<CacheWarmingDecision>& decision = std::nullopt, bool extensionOverride = false) {
        clearRun();
        m_inactive = CacheWarmingStatus{};
        m_inactive.reason = reason;
        m_inactive.decision = decision;
        m_inactive.extensionOverride = extensionOverride;
    }

    void clearRun() {
        if (m_run) {
            m_run->abort->abort();
            m_run.reset();
        }
    }

    /** Arms the timer thread for the next refresh; m_mutex is held. */
    void schedule(const std::shared_ptr<CacheWarmRun>& run) {
        const std::int64_t now = m_clock.nowMs();
        run->extensionOverride = false;
        run->nextWarmAt = now + run->delayMs;
        // A timer can run late after sleep or a blocked thread: keep half of the planned pre-expiry margin for that delay and
        // for dispatch, since a late refresh is likely a full-price cache write rather than a cache warm.
        run->refreshDeadlineAt = run->nextWarmAt + (run->ttlMs - run->delayMs) / 2;
        const std::int64_t deadline = run->startedAt + (run->phase == "idle" ? kMaxIdleAgeMs : kMaxAgeMs);
        if (run->nextWarmAt > deadline || now >= deadline) {
            stop(run->phase == "idle" ? "30-minute idle safety limit reached" : "one-hour safety limit reached");
            return;
        }
        run->timerArmed = true;
        ++m_threads;
        const std::int64_t wait = std::max<std::int64_t>(0, run->nextWarmAt - now);
        std::thread([this, run, wait] {
            if (m_sleeper.sleep(std::chrono::milliseconds(wait), run->abort)) {
                refresh(run);
            }
            const std::lock_guard<std::mutex> lock(m_mutex);
            --m_threads;
            m_changed.notify_all();
        }).detach();
    }

    void refresh(const std::shared_ptr<CacheWarmRun>& run) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            run->timerArmed = false;
            if (!validate(run) || deadlineMissed(run)) {
                return;
            }
        }
        const CacheWarmingDecision decision = evaluate(*run);
        std::string action = decision.action;
        if (m_decide) {
            action = m_decide(decision);
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!validate(run) || deadlineMissed(run)) {
                return;
            }
            const bool overridden = action != decision.action;
            if (action == "stop") {
                stop(overridden ? "stopped by extension" : (decision.economicsAvailable ? "expected savings below threshold" : "cache economics unavailable"), decision, overridden);
                return;
            }
            run->extensionOverride = overridden;
        }
        warm(run);
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_run == run) {
            schedule(run);
        }
    }

    /** Sends the one-token request and records its usage; failures are ignored (warming is best effort). */
    void warm(const std::shared_ptr<CacheWarmRun>& run) {
        StreamOptions options = run->request.options;
        options.maxTokens = 1;
        options.maxRetries = 0;
        options.signal = run->abort;
        const auto stream = m_models.stream(run->request.model, run->request.context, options);
        const std::optional<AssistantMessage> message = stream->result();
        Warmed warmed;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!validate(run) || !message || message->stopReason == StopReason::Error || message->stopReason == StopReason::Aborted) {
                return;
            }
            warmed = m_onWarmed;
        }
        const auto entry = m_session.appendUsage("cache_warm", message->provider, message->responseModel.value_or(message->model), message->usage,
                                                 run->extensionOverride ? std::optional<std::string>("extension override") : std::nullopt);
        if (entry && warmed) {
            warmed(*entry);
        }
    }

    bool deadlineMissed(const std::shared_ptr<CacheWarmRun>& run) {
        if (m_clock.nowMs() <= run->refreshDeadlineAt) {
            return false;
        }
        stop("cache refresh deadline missed");
        return true;
    }

    bool validate(const std::shared_ptr<CacheWarmRun>& run) {
        if (m_run != run) {
            return false;
        }
        std::optional<std::string> reason = modeStopReason(*run);
        if (!reason && run->isCurrent && !run->isCurrent()) {
            reason = "conversation context changed";
        }
        if (!reason) {
            return true;
        }
        stop(*reason);
        return false;
    }

    std::optional<std::string> modeStopReason(const CacheWarmRun& run) const {
        const std::string mode = m_mode();
        if (mode == "off") {
            return "cache warming disabled";
        }
        if (mode == "streaming" && run.phase == "idle") {
            return "agent run settled";
        }
        return std::nullopt;
    }

    /** Prices a refresh against the miss it would avoid. */
    CacheWarmingDecision evaluate(const CacheWarmRun& run) const {
        const Model& model = run.request.model;
        const std::int64_t promptTokens = lastPromptTokens();
        const double cacheHitCost = price(model, 0, 0, promptTokens, 0);
        const double cacheMissCost = model.cost.cacheWrite > 0 ? price(model, 0, 0, 0, promptTokens) : price(model, promptTokens, 0, 0, 0);
        CacheWarmingDecision decision;
        decision.phase = run.phase;
        decision.warmCost = price(model, 0, 1, promptTokens, 0);
        decision.missCost = std::max(0.0, cacheMissCost - cacheHitCost);
        decision.continuationProbability = run.phase == "idle" ? 0.15 : 1.0;
        decision.economicsAvailable = promptTokens > 0 && (cacheHitCost > 0 || cacheMissCost > 0);
        decision.expectedSavings = decision.continuationProbability * decision.missCost - decision.warmCost;
        decision.action = decision.expectedSavings >= 0.05 ? "warm" : "stop";
        return decision;
    }

    /** Prompt size of the most recent real request on the branch, as the provider reported it. */
    std::int64_t lastPromptTokens() const {
        const std::vector<SessionEntry> entries = m_session.branchPath();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->type == "message" && it->body.contains("message") && it->body["message"].value("role", std::string()) == "assistant" && it->body["message"].contains("usage")) {
                const Json& usage = it->body["message"]["usage"];
                return count(usage, "input") + count(usage, "cacheRead") + count(usage, "cacheWrite");
            }
        }
        return 0;
    }

    std::int64_t count(const Json& usage, const std::string& key) const {
        return usage.contains(key) && usage[key].is_number() ? usage[key].get<std::int64_t>() : 0;
    }

    double price(const Model& model, std::int64_t input, std::int64_t output, std::int64_t cacheRead, std::int64_t cacheWrite) const {
        Usage usage;
        usage.input = input;
        usage.output = output;
        usage.cacheRead = cacheRead;
        usage.cacheWrite = cacheWrite;
        return m_cost.calculate(model, usage).total;
    }

    IModelRuntime& m_models;
    ISessionManager& m_session;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    const IEnvironment& m_environment;
    Mode m_mode;
    Decide m_decide;
    CostCalculator m_cost;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::shared_ptr<CacheWarmRun> m_run;
    CacheWarmingStatus m_inactive;
    Warmed m_onWarmed;
    int m_threads = 0;
};
