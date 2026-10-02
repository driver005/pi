#pragma once

#include <string>

#include "interfaces/types/json/json.h"

/** Tool declaration shown to the model. `parameters` is a JSON Schema. */
struct Tool {
    std::string name;
    std::string description;
    Json parameters = Json::object();
    /** Constrained-sampling config; null means unset, `false` disables it. */
    Json constrainedSampling;
};
