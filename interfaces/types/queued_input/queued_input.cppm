export module pi.types.queued_input;

import std;

/** Texts of the messages waiting to be delivered to a running agent. */
export struct QueuedInput {
    std::vector<std::string> steering;
    std::vector<std::string> followUp;
};
