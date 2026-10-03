export module pi.support.compaction_preparer;

import std;
export import pi.support.agent_message_codec;
export import pi.support.agent_token_estimator;
export import pi.support.cut_point_finder;
export import pi.support.file_operation_tracker;
export import pi.support.session_projector;
export import pi.types.compaction_preparation;

/**
 * Decides what a compaction would summarize: the session path is projected into model context,
 * cut where about keepRecentTokens remain, and the older part (plus the earlier half of a split
 * turn) is collected with the file operations it contains. Port of prepareCompaction.
 */
export class CompactionPreparer {
public:
    /** nullopt when there is nothing to compact (path already ends in a compaction, or empty). */
    std::optional<CompactionPreparation> prepare(const std::vector<SessionEntry>& pathEntries,
                                                 const CompactionSettings& settings) const;

private:
    std::optional<std::size_t> previousCompaction(const std::vector<ProjectedSessionEntry>& entries) const;
    std::vector<AgentMessage> messagesOf(const std::vector<ProjectedSessionEntry>& entries,
                                         std::size_t from, std::size_t to) const;
    FileOperations fileOperations(const std::vector<AgentMessage>& messages,
                                  const SessionEntry* previous) const;

    AgentTokenEstimator m_estimator;
    CutPointFinder m_cuts;
    FileOperationTracker m_files;
    SessionProjector m_projector;
};

std::optional<std::size_t> CompactionPreparer::previousCompaction(
    const std::vector<ProjectedSessionEntry>& entries) const {
    // The newest compaction is projected first; older ones inside its kept range project empty.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].sourceEntry.type == "compaction" && !entries[i].messages.empty()) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<AgentMessage> CompactionPreparer::messagesOf(
    const std::vector<ProjectedSessionEntry>& entries, std::size_t from, std::size_t to) const {
    std::vector<AgentMessage> out;
    for (std::size_t i = from; i < to; ++i) {
        if (entries[i].sourceEntry.type == "compaction") {
            continue;
        }
        for (const auto& message : entries[i].messages) {
            // System messages are prompt state; the compaction entry carries their replay.
            if (!std::holds_alternative<SystemMessage>(message)) {
                out.push_back(message);
            }
        }
    }
    return out;
}

FileOperations CompactionPreparer::fileOperations(const std::vector<AgentMessage>& messages,
                                                  const SessionEntry* previous) const {
    FileOperations ops;
    // fromHook is kept for session file compatibility: summaries written by plugins carry
    // details of their own shape and are not trusted for file tracking.
    if (previous != nullptr && !previous->body.value("fromHook", false) &&
        previous->body.contains("details")) {
        m_files.seed(previous->body["details"], ops);
    }
    for (const auto& message : messages) {
        m_files.extract(message, ops);
    }
    return ops;
}

std::optional<CompactionPreparation> CompactionPreparer::prepare(
    const std::vector<SessionEntry>& pathEntries, const CompactionSettings& settings) const {
    if (pathEntries.empty() || pathEntries.back().type == "compaction") {
        return std::nullopt;
    }
    const SessionProjection projection = m_projector.project(pathEntries, pathEntries.back().id);
    const auto& entries = projection.entries;
    const auto previous = previousCompaction(entries);

    CompactionPreparation out;
    out.settings = settings;
    std::size_t boundaryStart = 0;
    if (previous) {
        out.previousSummary = entries[*previous].sourceEntry.body.value("summary", "");
        boundaryStart = *previous + 1;
    }
    out.tokensBefore = m_estimator.estimateProjected(projection, pathEntries).tokens;
    const CutPointResult cut = m_cuts.find(entries, boundaryStart, entries.size(), settings.keepRecentTokens);
    if (cut.firstKeptEntryIndex >= entries.size() || entries[cut.firstKeptEntryIndex].sourceEntry.id.empty()) {
        return std::nullopt;
    }
    out.firstKeptEntryId = entries[cut.firstKeptEntryIndex].sourceEntry.id;
    out.isSplitTurn = cut.isSplitTurn;
    const std::size_t historyEnd =
        cut.isSplitTurn ? static_cast<std::size_t>(cut.turnStartIndex) : cut.firstKeptEntryIndex;
    out.messagesToSummarize = messagesOf(entries, boundaryStart, historyEnd);
    if (cut.isSplitTurn) {
        out.turnPrefixMessages = messagesOf(entries, static_cast<std::size_t>(cut.turnStartIndex),
                                            cut.firstKeptEntryIndex);
    }
    if (out.messagesToSummarize.empty() && out.turnPrefixMessages.empty()) {
        return std::nullopt;
    }
    out.fileOps = fileOperations(out.messagesToSummarize,
                                 previous ? &entries[*previous].sourceEntry : nullptr);
    for (const auto& message : out.turnPrefixMessages) {
        m_files.extract(message, out.fileOps);
    }
    return out;
}
