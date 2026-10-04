export module pi.types.thinking_content;

import std;

/**
 * Model reasoning block. thinkingSignature is opaque provider replay data that must be sent
 * back unchanged; redacted blocks keep their encrypted payload there.
 */
export struct ThinkingContent {
    std::string thinking;
    std::optional<std::string> thinkingSignature;
    std::optional<bool> redacted;
};
