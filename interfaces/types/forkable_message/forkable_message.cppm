export module pi.types.forkable_message;

import std;

/** A user message a session can be forked or navigated from. */
export struct ForkableMessage {
    std::string entryId;
    std::string text;
};
