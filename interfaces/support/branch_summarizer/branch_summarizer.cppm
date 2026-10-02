module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.branch_summarizer;

import std;
export import pi.session.i_session_manager;
export import pi.support.agent_message_codec;
export import pi.support.agent_message_converter;
export import pi.support.agent_token_estimator;
export import pi.support.conversation_serializer;
export import pi.support.file_operation_tracker;
export import pi.support.summary_generator;
export import pi.types.branch_preparation;
export import pi.types.branch_summary_options;
export import pi.types.branch_summary_result;
export import pi.types.collect_entries_result;
export import pi.types.session_entry;

/**
 * Summarizes the branch a user leaves when navigating the session tree, so that context is not
 * lost: finds the abandoned entries, keeps as many recent ones as fit the budget and asks the
 * model for a structured summary. Port of compaction/branch-summarization.ts.
 */
export class BranchSummarizer {
public:
    explicit BranchSummarizer(const SummaryGenerator& generator);

    /** Entries from the old leaf back to (not including) the common ancestor with targetId. */
    CollectEntriesResult collectEntries(const ISessionManager& session,
                                        const std::optional<std::string>& oldLeafId,
                                        const std::string& targetId) const;

    /** Newest-first selection within tokenBudget (0 = unlimited), returned chronologically. */
    BranchPreparation prepare(const std::vector<SessionEntry>& entries, std::int64_t tokenBudget) const;

    BranchSummaryResult summarize(const std::vector<SessionEntry>& entries,
                                  const BranchSummaryOptions& options) const;

private:
    std::optional<AgentMessage> messageOf(const SessionEntry& entry) const;
    std::string stringField(const SessionEntry& entry, const char* key) const;
    std::string instructions(const BranchSummaryOptions& options) const;
    std::string preamble() const;
    std::string defaultPrompt() const;
    void seedFileOps(const std::vector<SessionEntry>& entries, FileOperations& ops) const;

    const SummaryGenerator& m_generator;
    AgentMessageCodec m_codec;
    AgentMessageConverter m_converter;
    AgentTokenEstimator m_estimator;
    ConversationSerializer m_serializer;
    FileOperationTracker m_files;
};

BranchSummarizer::BranchSummarizer(const SummaryGenerator& generator) : m_generator(generator) {}

CollectEntriesResult BranchSummarizer::collectEntries(const ISessionManager& session,
                                                      const std::optional<std::string>& oldLeafId,
                                                      const std::string& targetId) const {
    CollectEntriesResult out;
    if (!oldLeafId) {
        return out;
    }
    std::set<std::string> oldPath;
    for (const auto& entry : session.branchPath(oldLeafId)) {
        oldPath.insert(entry.id);
    }
    const std::vector<SessionEntry> targetPath = session.branchPath(targetId);
    for (auto it = targetPath.rbegin(); it != targetPath.rend(); ++it) {
        if (oldPath.contains(it->id)) {
            out.commonAncestorId = it->id;
            break;
        }
    }
    std::optional<std::string> current = oldLeafId;
    while (current && current != out.commonAncestorId) {
        const auto entry = session.entry(*current);
        if (!entry) {
            break;
        }
        out.entries.push_back(*entry);
        current = entry->parentId;
    }
    std::ranges::reverse(out.entries);
    return out;
}

std::string BranchSummarizer::stringField(const SessionEntry& entry, const char* key) const {
    const auto found = entry.body.find(key);
    return found != entry.body.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::optional<AgentMessage> BranchSummarizer::messageOf(const SessionEntry& entry) const {
    if (entry.type == "message") {
        if (!entry.body.contains("message")) {
            return std::nullopt;
        }
        const auto message = m_codec.fromJson(entry.body["message"]);
        // Tool results are skipped: their context is the assistant's tool call.
        if (!message || std::holds_alternative<ToolResultMessage>(*message)) {
            return std::nullopt;
        }
        return *message;
    }
    if (entry.type == "custom_message") {
        const Json content = entry.body.contains("content") ? entry.body["content"] : Json("");
        const Json details = entry.body.contains("details") ? entry.body["details"] : Json();
        return m_converter.custom(stringField(entry, "customType"), content,
                                  entry.body.value("display", true), details, entry.timestamp);
    }
    if (entry.type == "branch_summary") {
        const auto fromId = entry.body.find("fromId");
        std::optional<std::string> from;
        if (fromId != entry.body.end() && fromId->is_string()) {
            from = fromId->get<std::string>();
        }
        return m_converter.branchSummary(stringField(entry, "summary"), from, entry.timestamp);
    }
    if (entry.type == "compaction") {
        return m_converter.compactionSummary(stringField(entry, "summary"),
                                             entry.body.value("tokensBefore", std::int64_t{0}),
                                             entry.timestamp);
    }
    return std::nullopt;
}

void BranchSummarizer::seedFileOps(const std::vector<SessionEntry>& entries,
                                   FileOperations& ops) const {
    // Cumulative tracking through nested branch summaries; plugin-written ones are not trusted.
    for (const auto& entry : entries) {
        if (entry.type == "branch_summary" && !entry.body.value("fromHook", false) &&
            entry.body.contains("details")) {
            m_files.seed(entry.body["details"], ops);
        }
    }
}

BranchPreparation BranchSummarizer::prepare(const std::vector<SessionEntry>& entries,
                                            std::int64_t tokenBudget) const {
    BranchPreparation out;
    seedFileOps(entries, out.fileOps);
    std::deque<AgentMessage> chosen;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto message = messageOf(*it);
        if (!message) {
            continue;
        }
        m_files.extract(*message, out.fileOps);
        const std::int64_t tokens = m_estimator.messageTokens(*message);
        if (tokenBudget > 0 && out.totalTokens + tokens > tokenBudget) {
            // A summary entry is important context: keep it when there is still room.
            const bool isSummary = it->type == "compaction" || it->type == "branch_summary";
            if (isSummary && static_cast<double>(out.totalTokens) < static_cast<double>(tokenBudget) * 0.9) {
                chosen.push_front(*message);
                out.totalTokens += tokens;
            }
            break;
        }
        chosen.push_front(*message);
        out.totalTokens += tokens;
    }
    out.messages.assign(chosen.begin(), chosen.end());
    return out;
}

std::string BranchSummarizer::preamble() const {
    return "The user explored a different conversation branch before returning here.\nSummary of that exploration:\n\n";
}

std::string BranchSummarizer::defaultPrompt() const {
    return R"prompt(Create a structured summary of this conversation branch for context when returning later.

Use this EXACT format:

## Goal
[What was the user trying to accomplish in this branch?]

## Constraints & Preferences
- [Any constraints, preferences, or requirements mentioned]
- [Or "(none)" if none were mentioned]

## Progress
### Done
- [x] [Completed tasks/changes]

### In Progress
- [ ] [Work that was started but not finished]

### Blocked
- [Issues preventing progress, if any]

## Key Decisions
- **[Decision]**: [Brief rationale]

## Next Steps
1. [What should happen next to continue this work]

Keep each section concise. Preserve exact file paths, function names, and error messages.)prompt";
}

std::string BranchSummarizer::instructions(const BranchSummaryOptions& options) const {
    const bool custom = options.customInstructions && !options.customInstructions->empty();
    if (custom && options.replaceInstructions) {
        return *options.customInstructions;
    }
    if (custom) {
        return defaultPrompt() + "\n\nAdditional focus: " + *options.customInstructions;
    }
    return defaultPrompt();
}

BranchSummaryResult BranchSummarizer::summarize(const std::vector<SessionEntry>& entries,
                                                const BranchSummaryOptions& options) const {
    const Model& model = options.summarization.model;
    const std::int64_t contextWindow = model.contextWindow != 0 ? model.contextWindow : 128000;
    const BranchPreparation prepared = prepare(entries, contextWindow - options.reserveTokens);
    BranchSummaryResult out;
    if (prepared.messages.empty()) {
        out.summary = "No content to summarize";
        return out;
    }
    const std::string conversation = m_serializer.serialize(m_converter.convert(prepared.messages));
    const std::string prompt =
        "<conversation>\n" + conversation + "\n</conversation>\n\n" + instructions(options);
    StreamOptions request = options.summarization.stream;
    request.sessionId.reset();
    request.reasoning = ThinkingLevel::Off;
    request.maxTokens = model.maxTokens > 0 ? std::min<std::int64_t>(4096, model.maxTokens) : 4096;
    const AssistantMessage response = m_generator.complete(
        m_generator.buildContext(prompt), options.summarization, request);
    if (response.stopReason == StopReason::Aborted) {
        out.aborted = true;
        return out;
    }
    if (const auto problem = m_generator.failure(response, "Branch summarization")) {
        out.error = *problem;
        return out;
    }
    const bool calledTool = std::ranges::any_of(response.content, [](const AssistantContentBlock& block) {
        return std::holds_alternative<ToolCall>(block);
    });
    if (calledTool) {
        out.error = "Branch summarization attempted to call a tool";
        return out;
    }
    const FileLists lists = m_files.lists(prepared.fileOps);
    out.summary = preamble() + m_generator.text(response) + m_files.format(lists);
    out.usage = response.usage;
    out.readFiles = lists.readFiles;
    out.modifiedFiles = lists.modifiedFiles;
    return out;
}
