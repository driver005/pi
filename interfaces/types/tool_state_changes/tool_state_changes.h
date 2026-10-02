#pragma once

#include <vector>

#include "interfaces/types/tool/tool.h"
#include "interfaces/types/tool_reference/tool_reference.h"

/** Difference between two tool sets; a changed definition is a removal plus an addition. */
struct ToolStateChanges {
    std::vector<Tool> toolsAdded;
    std::vector<ToolReference> toolsRemoved;
};
