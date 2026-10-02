module;

#include <cstdint>

export module pi.support.summary_generator;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_id_generator;
export import pi.platform.i_sleeper;
export import pi.support.agent_message_converter;
export import pi.support.assistant_call_retrier;
export import pi.support.conversation_serializer;
export import pi.support.transcript_normalizer;
export import pi.types.agent_message;
export import pi.types.assistant_message;
export import pi.types.result;
export import pi.types.summarization_options;
export import pi.types.summary_result;
export import pi.types.transcript_context;

/**
 * Produces conversation summaries with one model call each: serializes the messages to text,
 * wraps them in the summarization prompt and sends a single-message request through the
 * session's stream function, retrying transient failures. Port of the summarization half of
 * compaction/compaction.ts.
 */
export class SummaryGenerator {
public:
    SummaryGenerator(const IClock& clock, IIdGenerator& ids, ISleeper& sleeper);

    /** The one call behind every summary: no caching, a routing session id, retry policy applied. */
    AssistantMessage complete(const TranscriptContext& context, const SummarizationOptions& options,
                              const StreamOptions& request) const;

    /** Summary of the messages, merged into previousSummary when given. */
    Result<SummaryResult> generate(const std::vector<AgentMessage>& messages, std::int64_t reserveTokens,
                                   const std::optional<std::string>& customInstructions,
                                   const std::optional<std::string>& previousSummary,
                                   const SummarizationOptions& options) const;

    /** Short checkpoint of the earlier part of a turn that compaction splits. */
    Result<SummaryResult> generateTurnPrefix(const std::vector<AgentMessage>& messages,
                                             std::int64_t reserveTokens,
                                             const SummarizationOptions& options) const;

    /** Why a response must not become a summary (error, length stop, tool call), or nullopt. */
    std::optional<std::string> failure(const AssistantMessage& response, const std::string& label) const;

    /** Context for a standalone request: the summarization system prompt and one user message. */
    TranscriptContext buildContext(const std::string& promptText) const;

    /** Joined text blocks of a response. */
    std::string text(const AssistantMessage& response) const;

private:
    StreamOptions requestOptions(const SummarizationOptions& options, std::int64_t maxTokens) const;
    Result<SummaryResult> finish(const AssistantMessage& response, const std::string& label) const;
    std::int64_t cappedTokens(const Model& model, std::int64_t budget) const;
    std::string summarizationPrompt() const;
    std::string updateInstructions() const;
    std::string turnPrefixPrompt() const;

    const IClock& m_clock;
    IIdGenerator& m_ids;
    AssistantCallRetrier m_retrier;
    AgentMessageConverter m_converter;
    ConversationSerializer m_serializer;
    TranscriptNormalizer m_normalizer;
};

SummaryGenerator::SummaryGenerator(const IClock& clock, IIdGenerator& ids, ISleeper& sleeper)
    : m_clock(clock), m_ids(ids), m_retrier(sleeper) {}

std::string SummaryGenerator::summarizationPrompt() const {
    return R"prompt(The messages above are a conversation to summarize. Create a structured context checkpoint summary that another LLM will use to continue the work.

Use this EXACT format:

## Goal
[What is the user trying to accomplish? Can be multiple items if the session covers different tasks.]

## Constraints & Preferences
- [Any constraints, preferences, or requirements mentioned by user]
- [Or "(none)" if none were mentioned]

## Progress
### Done
- [x] [Completed tasks/changes]

### In Progress
- [ ] [Current work]

### Blocked
- [Issues preventing progress, if any]

## Key Decisions
- **[Decision]**: [Brief rationale]

## Next Steps
1. [Ordered list of what should happen next]

## Critical Context
- [Any data, examples, or references needed to continue]
- [Or "(none)" if not applicable]

Keep each section concise. Preserve exact file paths, function names, and error messages.)prompt";
}

std::string SummaryGenerator::updateInstructions() const {
    return R"prompt(Update the existing structured summary with new information. RULES:
- PRESERVE all existing information from the previous summary
- ADD new progress, decisions, and context from the new messages
- UPDATE the Progress section: move items from "In Progress" to "Done" when completed
- UPDATE "Next Steps" based on what was accomplished
- PRESERVE exact file paths, function names, and error messages
- If something is no longer relevant, you may remove it

Use this EXACT format:

## Goal
[Preserve existing goals, add new ones if the task expanded]

## Constraints & Preferences
- [Preserve existing, add new ones discovered]

## Progress
### Done
- [x] [Include previously done items AND newly completed items]

### In Progress
- [ ] [Current work - update based on progress]

### Blocked
- [Current blockers - remove if resolved]

## Key Decisions
- **[Decision]**: [Brief rationale] (preserve all previous, add new)

## Next Steps
1. [Update based on current state]

## Critical Context
- [Preserve important context, add new if needed]

Keep each section concise. Preserve exact file paths, function names, and error messages.)prompt";
}

std::string SummaryGenerator::turnPrefixPrompt() const {
    return R"prompt(The messages above are earlier context from an ongoing conversation. Later messages are stored separately and do not need to be reconstructed.

Create a concise checkpoint of the user's request and the progress shown above. This checkpoint will be placed before the later messages so the conversation can continue with the necessary context.

## Original Request
[What did the user ask for?]

## Progress So Far
- [Key decisions and work completed in these messages]

## Context Needed to Continue
- [Information from these messages needed to understand the later work]

Only summarize information explicitly present above. Do not infer or recreate later messages.)prompt";
}

TranscriptContext SummaryGenerator::buildContext(const std::string& promptText) const {
    UserMessage user;
    user.content = std::vector<UserContentBlock>{TextContent{promptText, std::nullopt}};
    user.timestamp = m_clock.nowMs();
    Context context;
    context.systemPrompt = m_serializer.systemPrompt();
    context.messages.push_back(user);
    return m_normalizer.normalizeContext(context);
}

std::string SummaryGenerator::text(const AssistantMessage& response) const {
    std::string out;
    bool first = true;
    for (const auto& block : response.content) {
        if (const auto* part = std::get_if<TextContent>(&block)) {
            out += (first ? "" : "\n") + part->text;
            first = false;
        }
    }
    return out;
}

std::int64_t SummaryGenerator::cappedTokens(const Model& model, std::int64_t budget) const {
    return model.maxTokens > 0 ? std::min(budget, model.maxTokens) : budget;
}

StreamOptions SummaryGenerator::requestOptions(const SummarizationOptions& options,
                                               std::int64_t maxTokens) const {
    StreamOptions request = options.stream;
    request.maxTokens = maxTokens;
    if (options.model.reasoning && options.thinkingLevel != ThinkingLevel::Off) {
        request.reasoning = options.thinkingLevel;
    }
    return request;
}

AssistantMessage SummaryGenerator::complete(const TranscriptContext& context,
                                            const SummarizationOptions& options,
                                            const StreamOptions& request) const {
    // One-off summaries must not write cache entries. Keep caller routing when there is one;
    // otherwise (branch summaries) use a fresh routing id.
    StreamOptions effective = request;
    effective.cacheRetention = "none";
    if (!effective.sessionId) {
        effective.sessionId = m_ids.next();
    }
    const auto produce = [&]() -> AssistantMessage {
        AssistantMessage error;
        error.api = options.model.api;
        error.provider = options.model.provider;
        error.model = options.model.id;
        error.stopReason = StopReason::Error;
        error.timestamp = m_clock.nowMs();
        if (!options.streamFn) {
            error.errorMessage = "No stream function available for summarization";
            return error;
        }
        const auto stream = options.streamFn(options.model, context, effective);
        auto result = stream ? stream->result() : std::nullopt;
        if (!result) {
            error.errorMessage = "Summarization stream ended without a result";
            return error;
        }
        return *result;
    };
    return m_retrier.run(produce, options.retry, effective.signal, options.callbacks);
}

std::optional<std::string> SummaryGenerator::failure(const AssistantMessage& response,
                                                     const std::string& label) const {
    if (response.stopReason == StopReason::Error) {
        return label + " failed: " + response.errorMessage.value_or("Unknown error");
    }
    if (response.stopReason == StopReason::Length) {
        return label + " failed: generation hit the token cap and the summary is incomplete";
    }
    return std::nullopt;
}

Result<SummaryResult> SummaryGenerator::finish(const AssistantMessage& response,
                                               const std::string& label) const {
    if (response.stopReason == StopReason::Aborted) {
        return std::unexpected(Error{"aborted", label + " aborted"});
    }
    if (const auto problem = failure(response, label)) {
        return std::unexpected(Error{"summarization_failed", *problem});
    }
    const bool calledTool = std::ranges::any_of(response.content, [](const AssistantContentBlock& block) {
        return std::holds_alternative<ToolCall>(block);
    });
    if (calledTool) {
        return std::unexpected(Error{"summarization_failed", label + " attempted to call a tool"});
    }
    return SummaryResult{text(response), response.usage};
}

Result<SummaryResult> SummaryGenerator::generate(
    const std::vector<AgentMessage>& messages, std::int64_t reserveTokens,
    const std::optional<std::string>& customInstructions,
    const std::optional<std::string>& previousSummary, const SummarizationOptions& options) const {
    std::string base = previousSummary
                           ? "The messages above are NEW conversation messages to incorporate into the existing summary provided in <previous-summary> tags.\n\n" +
                                 updateInstructions()
                           : summarizationPrompt();
    if (customInstructions && !customInstructions->empty()) {
        base += "\n\nAdditional focus: " + *customInstructions;
    }
    const std::string conversation = m_serializer.serialize(m_converter.convert(messages));
    std::string prompt = "<conversation>\n" + conversation + "\n</conversation>\n\n";
    if (previousSummary) {
        prompt += "<previous-summary>\n" + *previousSummary + "\n</previous-summary>\n\n";
    }
    prompt += base;
    const std::int64_t maxTokens = cappedTokens(options.model, reserveTokens * 8 / 10);
    const AssistantMessage response =
        complete(buildContext(prompt), options, requestOptions(options, maxTokens));
    return finish(response, "Summarization");
}

Result<SummaryResult> SummaryGenerator::generateTurnPrefix(const std::vector<AgentMessage>& messages,
                                                           std::int64_t reserveTokens,
                                                           const SummarizationOptions& options) const {
    const std::string conversation = m_serializer.serialize(m_converter.convert(messages));
    const std::string prompt =
        "# Conversation\n" + conversation + "\n\n# Instructions\n" + turnPrefixPrompt();
    const std::int64_t maxTokens = cappedTokens(options.model, reserveTokens / 2);
    const AssistantMessage response =
        complete(buildContext(prompt), options, requestOptions(options, maxTokens));
    return finish(response, "Turn prefix summarization");
}
