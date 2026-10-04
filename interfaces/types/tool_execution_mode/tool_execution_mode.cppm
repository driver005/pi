export module pi.types.tool_execution_mode;

import std;

/** Sequential: one call at a time. Parallel: prepare in order, execute concurrently. */
export enum class ToolExecutionMode { Sequential, Parallel };
