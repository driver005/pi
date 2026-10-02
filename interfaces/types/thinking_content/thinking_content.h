#pragma once

#include <optional>
#include <string>

/**
 * Model reasoning block. thinkingSignature is opaque provider replay data that must be sent
 * back unchanged; redacted blocks keep their encrypted payload there.
 */
struct ThinkingContent {
    std::string thinking;
    std::optional<std::string> thinkingSignature;
    std::optional<bool> redacted;
};
