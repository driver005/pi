export module pi.types.responses_compat;

import std;

/** Resolved quirks of an OpenAI Responses endpoint. */
export struct ResponsesCompat {
    bool supportsDeveloperRole = true;
    /** Whether later system messages are sent in place; otherwise they fold into the leading prompt. */
    bool supportsMidConvoSystemMessages = false;
    /** "openai" | "openrouter". */
    std::string sessionAffinityFormat = "openai";
    bool supportsLongCacheRetention = true;
    bool supportsStrictMode = false;
    bool supportsExplicitPromptCacheMode = false;
    bool supportsMaxOutputTokens = true;
};
