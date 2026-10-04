export module pi.types.session_tree_node;

import std;
export import pi.types.session_entry;

/** Entry with its children, oldest first, and the label bookmarked on it. */
export struct SessionTreeNode {
    SessionEntry entry;
    std::vector<SessionTreeNode> children;
    std::optional<std::string> label;
    std::optional<std::string> labelTimestamp;
};
