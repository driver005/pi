module;

#include <nlohmann/json.hpp>

export module pi.types.tool;

import std;
export import pi.types.json;

/** Tool declaration shown to the model. `parameters` is a JSON Schema. */
export struct Tool {
    std::string name;
    std::string description;
    Json parameters = Json::object();
    /** Constrained-sampling config; null means unset, `false` disables it. */
    Json constrainedSampling;
};
