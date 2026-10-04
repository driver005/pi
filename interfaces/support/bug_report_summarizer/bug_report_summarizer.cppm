module;

#include <cstdint>

export module pi.support.bug_report_summarizer;

import std;
export import pi.support.summary_generator;
export import pi.types.agent_message;
export import pi.types.summarization_options;
import pi.support.agent_message_converter;
import pi.support.agent_token_estimator;
import pi.support.conversation_serializer;
import pi.support.transcript_normalizer;

/**
 * Asks the session's model to describe what went wrong, for a bug report whose transcript stays on the machine. The last
 * messages that fit 60% of the context window are serialized into one request; the answer must be plain text. Port of
 * generateBugReportSummary in core/bug-report.ts.
 */
export class BugReportSummarizer {
public:
    explicit BugReportSummarizer(const SummaryGenerator& generator)
        : m_generator(generator) {}

    Result<std::string> summarize(const std::vector<AgentMessage>& messages, const std::optional<std::string>& hint, const SummarizationOptions& options) const {
        const Model& model = options.model;
        const std::int64_t window = model.contextWindow > 0 ? model.contextWindow : 128'000;
        const std::vector<AgentMessage> selected = selectMessages(messages, window * 6 / 10);
        std::string note;
        if (selected.size() < messages.size()) {
            note = "Note: only the last " + std::to_string(selected.size()) + " of " + std::to_string(messages.size()) + " messages are shown.\n\n";
        }
        std::string prompt = note + "<conversation>\n" + m_serializer.serialize(m_converter.convert(selected)) + "\n</conversation>\n\n";
        const std::string trimmed = trim(hint.value_or(""));
        if (!trimmed.empty()) {
            prompt += "<user-report>\n" + trimmed + "\n</user-report>\n\n";
        }
        prompt += instructions();

        UserMessage user;
        user.content = std::vector<UserContentBlock>{TextContent{prompt, std::nullopt}};
        Context context;
        context.systemPrompt = systemPrompt();
        context.messages.push_back(user);
        StreamOptions request = options.stream;
        request.maxTokens = model.maxTokens > 0 ? std::min<std::int64_t>(4096, model.maxTokens) : 4096;
        if (model.reasoning && options.thinkingLevel != ThinkingLevel::Off) {
            request.reasoning = options.thinkingLevel;
        }
        const AssistantMessage response = m_generator.complete(m_normalizer.normalizeContext(context), options, request);
        if (response.stopReason == StopReason::Aborted) {
            return std::unexpected(Error{"aborted", "Bug report summary was cancelled"});
        }
        if (const auto failure = m_generator.failure(response, "Bug report summary")) {
            return std::unexpected(Error{"summary_failed", *failure});
        }
        for (const auto& block : response.content) {
            if (std::holds_alternative<ToolCall>(block)) {
                return std::unexpected(Error{"summary_failed", "Bug report summary attempted to call a tool"});
            }
        }
        const std::string text = trim(m_generator.text(response));
        if (text.empty()) {
            return std::unexpected(Error{"summary_failed", "Bug report summary was empty"});
        }
        return text;
    }

private:
    std::vector<AgentMessage> selectMessages(const std::vector<AgentMessage>& messages, std::int64_t tokenBudget) const {
        std::vector<AgentMessage> selected;
        std::int64_t tokens = 0;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            const std::int64_t next = m_estimator.messageTokens(*it);
            if (!selected.empty() && tokens + next > tokenBudget) {
                break;
            }
            selected.push_back(*it);
            tokens += next;
        }
        std::ranges::reverse(selected);
        return selected;
    }

    std::string systemPrompt() const {
        return "You are helping a user file a bug report about pi, the coding agent they are talking to. You will be shown the conversation transcript. Write a report for the pi developers describing what the user was doing and what went wrong.\n\nDo NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the report.";
    }

    std::string instructions() const {
        return "Write the bug report in Markdown with these sections:\n\n## What the user was doing\nOne short paragraph.\n\n## What went wrong\nConcrete description of the failure: wrong output, errors, hangs, tool failures, unexpected behavior. Quote error messages and tool output verbatim where they exist.\n\n## Steps to reproduce\nNumbered list, as specific as the transcript allows.\n\n## Relevant details\nTool calls involved, files touched, model behavior, anything else that helps a developer reproduce or locate the problem.\n\nDo not include file contents, secrets, or credentials from the transcript; refer to files by path only. Keep the report factual and concise.";
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    const SummaryGenerator& m_generator;
    AgentMessageConverter m_converter;
    AgentTokenEstimator m_estimator;
    ConversationSerializer m_serializer;
    TranscriptNormalizer m_normalizer;
};
