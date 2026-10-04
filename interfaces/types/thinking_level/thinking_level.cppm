export module pi.types.thinking_level;

import std;

/** Reasoning effort. Off is only meaningful at model/agent level; names are lowercase. */
export enum class ThinkingLevel { Off, Minimal, Low, Medium, High, XHigh, Max };
