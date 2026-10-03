export module pi.support.compactor;

import std;
export import pi.support.file_operation_tracker;
export import pi.support.summary_generator;
export import pi.support.usage_combiner;
export import pi.types.compaction_preparation;
export import pi.types.compaction_result;
export import pi.types.json;
export import pi.types.result;
export import pi.types.summarization_options;

/**
 * Runs a prepared compaction: summarizes the history (merging the previous summary), summarizes
 * the earlier part of a split turn separately, joins them and appends the file lists.
 * Port of compact() in compaction/compaction.ts.
 */
export class Compactor {
public:
    explicit Compactor(const SummaryGenerator& generator);

    Result<CompactionResult> compact(const CompactionPreparation& preparation,
                                     const std::optional<std::string>& customInstructions,
                                     const SummarizationOptions& options) const;

private:
    Result<SummaryResult> splitTurnSummary(const CompactionPreparation& preparation,
                                           const std::optional<std::string>& customInstructions,
                                           const SummarizationOptions& options) const;
    Result<SummaryResult> historySummary(const CompactionPreparation& preparation,
                                         const std::optional<std::string>& customInstructions,
                                         const SummarizationOptions& options) const;

    const SummaryGenerator& m_generator;
    FileOperationTracker m_files;
    UsageCombiner m_usage;
};

Compactor::Compactor(const SummaryGenerator& generator) : m_generator(generator) {}

Result<SummaryResult> Compactor::historySummary(const CompactionPreparation& preparation,
                                                const std::optional<std::string>& customInstructions,
                                                const SummarizationOptions& options) const {
    return m_generator.generate(preparation.messagesToSummarize, preparation.settings.reserveTokens,
                                customInstructions, preparation.previousSummary, options);
}

Result<SummaryResult> Compactor::splitTurnSummary(
    const CompactionPreparation& preparation, const std::optional<std::string>& customInstructions,
    const SummarizationOptions& options) const {
    std::string historyText = preparation.previousSummary.value_or("No prior history.");
    std::optional<Usage> historyUsage;
    if (!preparation.messagesToSummarize.empty()) {
        auto history = historySummary(preparation, customInstructions, options);
        if (!history) {
            return history;
        }
        historyText = history->text;
        historyUsage = history->usage;
    }
    auto prefix = m_generator.generateTurnPrefix(preparation.turnPrefixMessages,
                                                 preparation.settings.reserveTokens, options);
    if (!prefix) {
        return prefix;
    }
    SummaryResult out;
    out.text = historyText + "\n\n---\n\n**Turn Context (split turn):**\n\n" + prefix->text;
    out.usage = historyUsage ? m_usage.combine(*historyUsage, prefix->usage) : prefix->usage;
    return out;
}

Result<CompactionResult> Compactor::compact(const CompactionPreparation& preparation,
                                            const std::optional<std::string>& customInstructions,
                                            const SummarizationOptions& options) const {
    if (preparation.firstKeptEntryId.empty()) {
        return std::unexpected(
            Error{"invalid_preparation", "First kept entry has no UUID - session may need migration"});
    }
    auto summary = preparation.isSplitTurn && !preparation.turnPrefixMessages.empty()
                       ? splitTurnSummary(preparation, customInstructions, options)
                       : historySummary(preparation, customInstructions, options);
    if (!summary) {
        return std::unexpected(summary.error());
    }
    const FileLists lists = m_files.lists(preparation.fileOps);
    CompactionResult out;
    out.summary = summary->text + m_files.format(lists);
    out.firstKeptEntryId = preparation.firstKeptEntryId;
    out.tokensBefore = preparation.tokensBefore;
    out.usage = summary->usage;
    out.details = Json{{"readFiles", lists.readFiles}, {"modifiedFiles", lists.modifiedFiles}};
    return out;
}
