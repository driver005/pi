module;

#include <cstdint>

export module pi.types.view_observation;

import std;
export import pi.types.json;

/** A registered observer of a conversation view: the view as the observer starts from it, and the id to unobserve with. */
export struct ViewObservation {
    Json value;
    std::int64_t id = 0;
};
