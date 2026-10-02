module;
#include <nlohmann/json.hpp>

export module pi.tools.grep_tool;

import std;
import pi.platform.i_file_system;
import pi.platform.i_process_runner;
import pi.support.path_resolver;
import pi.support.text_truncator;
import pi.support.tool_result_factory;
export import pi.tool.i_tool;

/**
 * The `grep` tool (port of core/tools/grep.ts): runs ripgrep with --json, stops it once the match
 * limit is reached, and formats matches (with optional context) relative to the search directory.
 */
export class GrepTool : public ITool {
public:
    static constexpr std::int64_t kDefaultLimit = 100;

    GrepTool(IProcessRunner& runner, IFileSystem& fileSystem, std::string cwd)
        : m_runner(runner), m_fileSystem(fileSystem), m_cwd(std::move(cwd)), m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "grep";
        m_definition.description =
            "Search file contents for a pattern. Returns matching lines with file paths and line numbers. "
            "Respects .gitignore. Output is truncated to 100 matches or 50KB (whichever is hit first). "
            "Long lines are truncated to 500 chars.";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "pattern":{"type":"string","description":"Search pattern (regex or literal string)"},
                "path":{"type":"string","description":"Directory or file to search (default: current directory)"},
                "glob":{"type":"string","description":"Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'"},
                "ignoreCase":{"type":"boolean","description":"Case-insensitive search (default: false)"},
                "literal":{"type":"boolean","description":"Treat pattern as literal string instead of regex (default: false)"},
                "context":{"type":"number","description":"Number of lines to show before and after each match (default: 0)"},
                "limit":{"type":"number","description":"Maximum number of matches to return (default: 100)"}
            },
            "required":["pattern"]})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "grep";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }

    Result<AgentToolResult> execute(const std::string&, const Json& params,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback&) override {
        if (signal != nullptr && signal->aborted()) {
            return std::unexpected(Error{"aborted", "Operation aborted"});
        }
        const std::string requested = params.value("path", std::string());
        const std::string searchPath = m_paths.resolveToCwd(requested.empty() ? "." : requested, m_cwd);
        const auto info = m_fileSystem.stat(searchPath);
        if (!info.has_value()) {
            return std::unexpected(Error{"not_found", "Path not found: " + searchPath});
        }
        const std::int64_t limit = std::max<std::int64_t>(
            1, params.contains("limit") && params["limit"].is_number()
                   ? static_cast<std::int64_t>(params["limit"].get<double>())
                   : kDefaultLimit);
        const std::int64_t context = params.contains("context") && params["context"].is_number()
                                         ? std::max<std::int64_t>(0, static_cast<std::int64_t>(params["context"].get<double>()))
                                         : 0;
        return search(params, searchPath, info->isDirectory, limit, context, signal);
    }

private:
    std::vector<std::string> buildArgs(const Json& params, const std::string& searchPath) const {
        std::vector<std::string> args = {"--json", "--line-number", "--color=never", "--hidden"};
        if (params.value("ignoreCase", false)) {
            args.push_back("--ignore-case");
        }
        if (params.value("literal", false)) {
            args.push_back("--fixed-strings");
        }
        if (const std::string glob = params.value("glob", std::string()); !glob.empty()) {
            args.push_back("--glob");
            args.push_back(glob);
        }
        args.push_back("--");
        args.push_back(params["pattern"].get<std::string>());
        args.push_back(searchPath);
        return args;
    }

    std::string formatPath(const std::string& filePath, const std::string& searchPath, bool isDirectory) const {
        if (isDirectory) {
            const std::string relative = m_paths.relativeTo(filePath, searchPath);
            if (!relative.empty() && relative != filePath) {
                return relative;
            }
        }
        return std::filesystem::path(filePath).filename().string();
    }

    std::vector<std::string> fileLines(const std::string& path) const {
        auto content = m_fileSystem.readFile(path);
        if (!content.has_value()) {
            return {};
        }
        std::string normalized;
        for (std::size_t i = 0; i < content->size(); ++i) {
            if ((*content)[i] == '\r') {
                normalized.push_back('\n');
                i += (i + 1 < content->size() && (*content)[i + 1] == '\n') ? 1 : 0;
            } else {
                normalized.push_back((*content)[i]);
            }
        }
        return m_results.splitLines(normalized);
    }

    Result<AgentToolResult> search(const Json& params, const std::string& searchPath, bool isDirectory,
                                   std::int64_t limit, std::int64_t context,
                                   const std::shared_ptr<AbortSignal>& signal) {
        std::vector<std::tuple<std::string, std::int64_t, std::optional<std::string>>> matches;
        std::string pending;
        std::string stderrText;
        bool limitReached = false;
        auto local = std::make_shared<AbortSignal>();
        std::uint64_t listener = 0;
        if (signal != nullptr) {
            listener = signal->onAbort([local] { local->abort(); });
        }
        const auto handleLine = [&](const std::string& line) {
            if (line.find_first_not_of(" \t\r") == std::string::npos || limitReached) {
                return;
            }
            const Json event = Json::parse(line, nullptr, false);
            if (event.is_discarded() || !event.is_object()) {
                stderrText += line + "\n";
                return;
            }
            if (event.value("type", "") != "match" || !event.contains("data")) {
                return;
            }
            const Json& data = event["data"];
            if (!data.contains("path") || !data["path"].contains("text") || !data.contains("line_number")) {
                return;
            }
            std::optional<std::string> lineText;
            if (data.contains("lines") && data["lines"].contains("text")) {
                lineText = data["lines"]["text"].get<std::string>();
            }
            matches.emplace_back(data["path"]["text"].get<std::string>(), data["line_number"].get<std::int64_t>(), lineText);
            if (static_cast<std::int64_t>(matches.size()) >= limit) {
                limitReached = true;
                local->abort();
            }
        };
        ProcessRequest request;
        request.command = "rg";
        request.args = buildArgs(params, searchPath);
        request.signal = local;
        request.captureOutput = false;
        request.onOutput = [&](std::string_view chunk) {
            pending.append(chunk);
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                handleLine(pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
        };
        auto outcome = m_runner.run(request);
        if (signal != nullptr) {
            signal->removeListener(listener);
        }
        if (!pending.empty()) {
            handleLine(pending);
        }
        if (!outcome.has_value()) {
            return std::unexpected(Error{"spawn", "Failed to run ripgrep: " + outcome.error().message});
        }
        if (signal != nullptr && signal->aborted()) {
            return std::unexpected(Error{"aborted", "Operation aborted"});
        }
        if (!limitReached && outcome->exitCode != 0 && outcome->exitCode != 1) {
            const std::string trimmed = stderrText.substr(0, stderrText.find_last_not_of(" \n\t") + 1);
            return std::unexpected(Error{"rg", trimmed.empty() ? "ripgrep exited with code " + std::to_string(outcome->exitCode) : trimmed});
        }
        if (matches.empty()) {
            return m_results.text("No matches found");
        }
        return format(matches, searchPath, isDirectory, limit, context, limitReached);
    }

    Result<AgentToolResult> format(
        const std::vector<std::tuple<std::string, std::int64_t, std::optional<std::string>>>& matches,
        const std::string& searchPath, bool isDirectory, std::int64_t limit, std::int64_t context,
        bool limitReached) const {
        std::vector<std::string> lines;
        bool linesTruncated = false;
        std::map<std::string, std::vector<std::string>> cache;
        for (const auto& [filePath, lineNumber, lineText] : matches) {
            const std::string relative = formatPath(filePath, searchPath, isDirectory);
            if (context == 0 && lineText.has_value()) {
                std::string sanitized = *lineText;
                while (!sanitized.empty() && (sanitized.back() == '\n' || sanitized.back() == '\r')) {
                    sanitized.pop_back();
                }
                const auto [text, cut] = m_truncator.truncateLine(sanitized);
                linesTruncated = linesTruncated || cut;
                lines.push_back(relative + ":" + std::to_string(lineNumber) + ": " + text);
                continue;
            }
            if (!cache.contains(filePath)) {
                cache[filePath] = fileLines(filePath);
            }
            const std::vector<std::string>& content = cache[filePath];
            if (content.empty()) {
                lines.push_back(relative + ":" + std::to_string(lineNumber) + ": (unable to read file)");
                continue;
            }
            const std::int64_t start = context > 0 ? std::max<std::int64_t>(1, lineNumber - context) : lineNumber;
            const std::int64_t end = context > 0 ? std::min<std::int64_t>(static_cast<std::int64_t>(content.size()), lineNumber + context) : lineNumber;
            for (std::int64_t current = start; current <= end; ++current) {
                std::string text = current - 1 < static_cast<std::int64_t>(content.size()) ? content[static_cast<std::size_t>(current - 1)] : "";
                text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
                const auto [shown, cut] = m_truncator.truncateLine(text);
                linesTruncated = linesTruncated || cut;
                lines.push_back(relative + (current == lineNumber ? ":" : "-") + std::to_string(current) +
                                (current == lineNumber ? ": " : "- ") + shown);
            }
        }
        const TruncationResult truncation =
            m_truncator.truncateHead(m_results.join(lines, "\n"), std::numeric_limits<std::int64_t>::max());
        std::string output = truncation.content;
        Json details = Json::object();
        std::vector<std::string> notices;
        if (limitReached) {
            notices.push_back(std::to_string(limit) + " matches limit reached. Use limit=" + std::to_string(limit * 2) +
                              " for more, or refine pattern");
            details["matchLimitReached"] = limit;
        }
        if (truncation.truncated) {
            notices.push_back(m_truncator.formatSize(TextTruncator::kDefaultMaxBytes) + " limit reached");
            details["truncation"] = m_results.truncationJson(truncation);
        }
        if (linesTruncated) {
            notices.push_back("Some lines truncated to " + std::to_string(TextTruncator::kGrepMaxLineLength) +
                              " chars. Use read tool to see full lines");
            details["linesTruncated"] = true;
        }
        if (!notices.empty()) {
            output += "\n\n[" + m_results.join(notices, ". ") + "]";
        }
        return m_results.text(output, details.empty() ? Json() : details);
    }

    IProcessRunner& m_runner;
    IFileSystem& m_fileSystem;
    std::string m_cwd;
    PathResolver m_paths;
    TextTruncator m_truncator;
    ToolResultFactory m_results;
    Tool m_definition;
};
