export module pi.types.submission_draft;

import std;
export import pi.types.json;

/** Host submission: user input that may start a run, or a passive entry write. */
export struct SubmissionDraft {
    std::optional<std::string> requestId;
    /** "input" or "write". */
    std::string type = "input";
    /** input: the user message content (a string or an array of content blocks). */
    Json content;
    /** input: "steer", "followUp" (default) or "reject" while busy. */
    std::optional<std::string> whenBusy;
    /** write: an entry draft. */
    Json entry;
};
