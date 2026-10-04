export module pi.types.navigate_tree_result;

import std;
export import pi.types.session_entry;

export struct NavigateTreeResult {
    /** Text of the user message navigated to, for the client to edit and resubmit. */
    std::optional<std::string> editorText;
    bool cancelled = false;
    bool aborted = false;
    std::optional<SessionEntry> summaryEntry;
};
