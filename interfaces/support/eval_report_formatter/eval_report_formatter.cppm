export module pi.support.eval_report_formatter;

import std;
export import pi.types.json;
import pi.support.js_number_format;

/**
 * Renders the report of EvalReportSummarizer as the text the runner prints: per eval set the eligible pairs, the pass-rate
 * lift in percentage points (withheld while pairs are blocked), the flags and the paired deltas of tokens, tool calls, latency and
 * estimated cost, then the operational totals per variant and the blocked pairs with their reasons. Port of
 * formatEvalComparisonReport in packages/evals/src/report.ts, without the bold styling of the title. Empty for a report without comparisons.
 */
export class EvalReportFormatter {
public:
    std::string format(const Json& report) const {
        const Json& comparisons = report["comparisons"];
        if (comparisons.empty()) {
            return "";
        }
        std::vector<std::string> lines{"Documentation Eval Comparisons"};
        for (const Json& comparison : comparisons) {
            addComparison(comparison, lines);
        }
        lines.push_back("  Operational totals");
        for (const Json& totals : report["operationalTotals"]) {
            const int runs = totals["runs"].get<int>();
            lines.push_back("    " + totals["variant"].get<std::string>() + ": " + std::to_string(runs) + " runs, " +
                            operational(totals["totalTokens"], runs, [this](double total) { return fixedOrInteger(total) + " tokens"; }) + ", " +
                            operational(totals["toolCalls"], runs, [this](double total) { return fixedOrInteger(total) + " tools"; }) + ", " +
                            operational(totals["totalMs"], runs, [this](double total) { return m_format.toFixed(total / 1000, 2) + "s"; }) + ", " +
                            operational(totals["estimatedCostUsd"], runs, [this](double total) { return "$" + m_format.toFixed(total, 4) + " cost"; }));
        }
        if (!report["blockedPairs"].empty()) {
            lines.push_back("  Blocked pairs");
            for (const Json& blocked : report["blockedPairs"]) {
                std::string reasons;
                for (const Json& reason : blocked["reasons"]) {
                    reasons += (reasons.empty() ? "" : "; ") + reason.get<std::string>();
                }
                lines.push_back("    " + blocked["evalSet"].get<std::string>() + "/" + blocked["caseId"].get<std::string>() + "/" + blocked["model"].get<std::string>() + "/run-" + std::to_string(blocked["runNumber"].get<std::int64_t>()) + ": " + reasons);
            }
        }
        std::string out;
        for (const std::string& line : lines) {
            out += (out.empty() ? "" : "\n") + line;
        }
        return out;
    }

private:
    void addComparison(const Json& comparison, std::vector<std::string>& lines) const {
        lines.push_back("  " + comparison["evalSet"].get<std::string>());
        lines.push_back("         Pairs  " + std::to_string(comparison["eligiblePairs"].get<int>()) + "/" + std::to_string(comparison["totalPairs"].get<int>()) + " eligible");
        if (comparison["lift"].is_null()) {
            lines.push_back(comparison["blockedPairs"].get<int>() > 0 ? "     Pass rate  withheld because pairs are blocked" : "     Pass rate  unavailable");
        } else {
            lines.push_back("     Pass rate  " + withSign(comparison["lift"].get<double>() * 100, 1) + " pp (with " + percentage(comparison["treatmentPassRate"]) + ", without " + percentage(comparison["controlPassRate"]) + ")");
        }
        if (!comparison["flags"].empty()) {
            std::string flags;
            for (const Json& flag : comparison["flags"]) {
                flags += (flags.empty() ? "" : ", ") + flag.get<std::string>();
            }
            lines.push_back("         Flags  " + flags);
        }
        lines.push_back(pairedMetric("Tokens", comparison["totalTokens"], ""));
        lines.push_back(pairedMetric("Tools", comparison["toolCalls"], ""));
        lines.push_back(pairedMetric("Latency", comparison["totalMs"], "ms"));
        const Json& cost = comparison["estimatedCostUsd"];
        if (cost["meanDelta"].is_null() || cost["controlMean"].is_null() || cost["treatmentMean"].is_null()) {
            lines.push_back("     Est. cost  unavailable");
        } else {
            const double delta = cost["meanDelta"].get<double>();
            lines.push_back(std::string("     Est. cost  ") + (delta >= 0 ? "+" : "-") + "$" + m_format.toFixed(std::abs(delta), 4) + " (with $" + m_format.toFixed(cost["treatmentMean"].get<double>(), 4) + ", without $" +
                            m_format.toFixed(cost["controlMean"].get<double>(), 4) + ", " + std::to_string(cost["eligiblePairs"].get<int>()) + " pairs)");
        }
    }

    std::string percentage(const Json& value) const {
        return value.is_null() ? "unavailable" : m_format.toFixed(value.get<double>() * 100, 1) + "%";
    }

    std::string withSign(double value, int digits) const {
        return std::string(value >= 0 ? "+" : "") + m_format.toFixed(value, digits);
    }

    std::string pairedMetric(const std::string& label, const Json& metric, const std::string& unit) const {
        const std::string padded = std::string(label.size() < 10 ? 10 - label.size() : 0, ' ') + label;
        if (metric["meanDelta"].is_null() || metric["controlMean"].is_null() || metric["treatmentMean"].is_null()) {
            return "    " + padded + "  unavailable";
        }
        return "    " + padded + "  " + withSign(metric["meanDelta"].get<double>(), 1) + unit + " (with " + m_format.toFixed(metric["treatmentMean"].get<double>(), 1) + unit + ", without " + m_format.toFixed(metric["controlMean"].get<double>(), 1) + unit + ", " +
               std::to_string(metric["eligiblePairs"].get<int>()) + " pairs)";
    }

    std::string operational(const Json& metric, int runs, const std::function<std::string(double)>& render) const {
        if (metric["total"].is_null()) {
            return "unavailable (0/" + std::to_string(runs) + " measured)";
        }
        const int available = metric["availableRuns"].get<int>();
        return render(metric["total"].get<double>()) + (available == runs ? "" : " (" + std::to_string(available) + "/" + std::to_string(runs) + " measured)");
    }

    /** A count the way JavaScript prints a number: no decimals for a whole value. */
    std::string fixedOrInteger(double value) const {
        return value == std::floor(value) && std::abs(value) < 1e15 ? std::format("{}", static_cast<std::int64_t>(value)) : std::format("{}", value);
    }

    JsNumberFormat m_format;
};
