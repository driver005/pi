module;

#include <cstdint>

export module pi.types.ownership_ref;

/** Where a walk up the ownership tree continues: an owner task, or a conversation. */
export struct OwnershipRef {
    bool task = false;
    std::int64_t id = 0;
};
