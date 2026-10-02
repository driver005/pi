#pragma once

#include <nlohmann/json.hpp>

/** JSON document type (insertion-ordered, like JS objects; key order matters for prompt caching). */
using Json = nlohmann::ordered_json;
