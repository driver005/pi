export module pi.types.fork_parent;

import std;
export import pi.types.json;

/** The conversation a fork was taken from and the newest of its entries the fork can see. */
export struct ForkParent {
    Json record;
    std::int64_t at = 0;
};
