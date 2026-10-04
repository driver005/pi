export module pi.types.prompt_disposition;

import std;

/** What happened to a submitted prompt: it started a run, or was queued behind the active one. */
/** Started: a run began; Queued: held for the running agent; Handled: a plugin consumed the input. */
export enum class PromptDisposition { Started, Queued, Handled };
