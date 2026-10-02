module;
#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.model;

import std;
export import pi.types.json;
export import pi.types.model_cost;

/**
 * Chat model catalog entry. Provider-specific knobs (thinkingLevelMap, compat, inputLimits,
 * promptCache, samplingParams) stay JSON; each wire-API module reads the keys it understands.
 */
export struct Model {
    std::string id;
    std::string name;
    std::string api;
    std::string provider;
    std::string baseUrl;
    /** Accepted input modalities: "text" and/or "image". */
    std::vector<std::string> input = {"text"};
    Json inputLimits;
    ModelCost cost;
    std::map<std::string, std::string> headers;
    bool reasoning = false;
    Json thinkingLevelMap;
    Json promptCache;
    std::int64_t contextWindow = 0;
    std::int64_t maxTokens = 0;
    Json samplingParams;
    Json compat;
};
