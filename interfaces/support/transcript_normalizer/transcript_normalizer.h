#pragma once

#include <optional>
#include <string>
#include <vector>

#include "interfaces/types/context/context.h"
#include "interfaces/types/message/message.h"
#include "interfaces/types/system_message/system_message.h"
#include "interfaces/types/tool/tool.h"
#include "interfaces/types/tool_state_changes/tool_state_changes.h"
#include "interfaces/types/transcript_context/transcript_context.h"

/**
 * Transcript helpers: folding a Context into leading system messages, replaying system
 * messages into the current prompt and tool set, and diffing tool sets.
 * Port of packages/ai/src/utils/transcript.ts and the text helpers in utils/text.ts.
 */
class TranscriptNormalizer {
public:
    /** Leading system message for a prompt and tool set; nullopt when both are empty. */
    std::optional<SystemMessage> createInitialSystemMessage(
        const std::optional<std::string>& systemPrompt,
        const std::optional<std::vector<Tool>>& tools) const;

    /** Folds Context::systemPrompt and Context::tools into a leading system message. */
    TranscriptContext normalizeContext(const Context& context) const;

    /** The leading system message when the transcript starts with one. */
    const SystemMessage* initialSystemMessage(const std::vector<Message>& messages) const;

    /** Tools available after applying every system message in order. */
    std::vector<Tool> currentTools(const std::vector<Message>& messages) const;

    /** All system messages replayed into one leading message (content joined by blank lines). */
    std::optional<SystemMessage> currentSystemMessage(const std::vector<Message>& messages) const;

    std::string currentSystemPrompt(const std::vector<Message>& messages) const;

    /** For APIs without mid-conversation system messages: replayed head, later ones dropped. */
    TranscriptContext collapseSystemMessages(const TranscriptContext& context) const;

    TranscriptContext resolveTranscript(const TranscriptContext& context,
                                        bool supportsMidConvoSystemMessages) const;

    Tool toToolDeclaration(const Tool& tool) const;
    bool declarationsEqual(const Tool& left, const Tool& right) const;
    ToolStateChanges toolStateChanges(const std::vector<Tool>& previous,
                                      const std::vector<Tool>& current) const;

    /** Joined text blocks of a system/user content value. */
    std::string systemContentText(const SystemMessage& message) const;
    /** A system message as a complete prompt: content followed by its sections. */
    std::string systemMessageText(const SystemMessage& message) const;
    /** Later system message rendered for mid-conversation delivery (sections framed by name). */
    std::string renderSystemMessageUpdate(const SystemMessage& message) const;
};
