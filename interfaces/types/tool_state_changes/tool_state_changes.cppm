export module pi.types.tool_state_changes;

import std;
export import pi.types.tool;
export import pi.types.tool_reference;

/** Difference between two tool sets; a changed definition is a removal plus an addition. */
export struct ToolStateChanges {
    std::vector<Tool> toolsAdded;
    std::vector<ToolReference> toolsRemoved;
};
