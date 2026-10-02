#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "interfaces/types/json/json.h"
#include "interfaces/types/model_cost/model_cost.h"

/**
 * Chat model catalog entry. Provider-specific knobs (thinkingLevelMap, compat, inputLimits,
 * promptCache, samplingParams) stay JSON; each wire-API module reads the keys it understands.
 */
struct Model {
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
