export module pi.types.fork_result;

import std;

/** Where a fork cuts: before a user message (the usual) or at the chosen entry. */
export enum class ForkPosition { Before, At };

export struct ForkResult {
    /** Text of the user message forked from, for the client to edit and resubmit. */
    std::optional<std::string> selectedText;
};
