export module pi.types.cache_warming_decision;

import std;

/** Inputs and outcome of one warm-or-stop decision. */
export struct CacheWarmingDecision {
    /** "streaming" while the agent run that sent the request is still active, else "idle". */
    std::string phase = "streaming";
    /** Price of this refresh: a cache read of the prompt plus one output token. */
    double warmCost = 0;
    /** Extra price of the next real request if the cache entry is lost. */
    double missCost = 0;
    /** Estimated chance that a real request arrives before the entry expires. */
    double continuationProbability = 0;
    /** `continuationProbability * missCost - warmCost`. */
    double expectedSavings = 0;
    /** False when the prompt size or the model's prices are unknown. */
    bool economicsAvailable = false;
    /** "warm" when `expectedSavings` is at least $0.05, else "stop"; a plugin may override it. */
    std::string action = "stop";
};
