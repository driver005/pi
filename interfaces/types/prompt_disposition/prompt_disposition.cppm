export module pi.types.prompt_disposition;

import std;

/** What happened to a submitted prompt: it started a run, or was queued behind the active one. */
export enum class PromptDisposition { Started, Queued };
