export module pi.types.storage_page;

import std;
export import pi.types.json;

/** One ordered scan result and the opaque cursor that continues it (absent on the last page). */
export struct StoragePage {
    std::vector<Json> items;
    std::optional<Json> next;
};
