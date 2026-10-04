module;

#include <nlohmann/json.hpp>

export module pi.types.json;

import std;

/** JSON document type (insertion-ordered, like JS objects; key order matters for prompt caching). */
export using Json = nlohmann::ordered_json;
