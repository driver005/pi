module;

#include <cstdint>

export module pi.types.doc_address_args;

import std;
export import pi.types.json;

/** The arguments that name one document: its owner (conversation or task id), family key and seed. */
export struct DocAddressArgs {
    std::optional<std::int64_t> owner;
    std::optional<std::string> key;
    Json seed;
};
