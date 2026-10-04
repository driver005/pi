export module pi.types.queued_entry;

import std;
export import pi.types.image_content;

/** Input a controller queued behind an active run, kept so it can be withdrawn without losing the rest. */
export struct QueuedEntry {
    std::string id;
    bool steering = false;
    std::string text;
    std::vector<ImageContent> images;
};
