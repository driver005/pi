module;

#include <cstdint>

export module pi.types.idle_scope;

/** Where ordinary ownership traversal starts: one conversation, or every ownerless conversation. */
export struct IdleScope {
    bool roots = false;
    std::int64_t conversation = 0;
};
