module;
#include <nlohmann/json.hpp>

export module pi.tools.ls_tool;

import std;
import pi.platform.i_file_system;
import pi.support.path_resolver;
import pi.support.text_truncator;
import pi.support.tool_result_factory;
export import pi.tool.i_tool;

/** The `ls` tool: alphabetical directory listing with '/' suffix for directories. */
export class LsTool : public ITool {
public:
    static constexpr std::int64_t kDefaultLimit = 500;

    LsTool(IFileSystem& fileSystem, std::string cwd)
        : m_fileSystem(fileSystem), m_cwd(std::move(cwd)), m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "ls";
        m_definition.description =
            "List directory contents. Returns entries sorted alphabetically, with '/' suffix for "
            "directories. Includes dotfiles. Output is truncated to 500 entries or 50KB (whichever is hit first).";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Directory to list (default: current directory)"},
                "limit":{"type":"number","description":"Maximum number of entries to return (default: 500)"}
            }})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "ls";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    std::string promptSnippet() const override {
        return "List directory contents";
    }
    std::vector<std::string> promptGuidelines() const override {
        return {};
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
        const std::string dir = m_paths.resolveToCwd(requested.empty() ? "." : requested, m_cwd);
        const std::int64_t limit = params.contains("limit") && params["limit"].is_number()
                                       ? static_cast<std::int64_t>(params["limit"].get<double>())
                                       : kDefaultLimit;
        if (!m_fileSystem.exists(dir)) {
            return std::unexpected(Error{"not_found", "Path not found: " + dir});
        }
        const auto info = m_fileSystem.stat(dir);
        if (!info.has_value() || !info->isDirectory) {
            return std::unexpected(Error{"not_directory", "Not a directory: " + dir});
        }
        auto entries = m_fileSystem.listDirectory(dir);
        if (!entries.has_value()) {
            return std::unexpected(Error{"read_dir", "Cannot read directory: " + entries.error().message});
        }
        return format(dir, sorted(std::move(*entries)), limit);
    }

private:
    std::string lower(std::string text) const {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::vector<std::string> sorted(std::vector<std::string> entries) const {
        std::sort(entries.begin(), entries.end(), [this](const std::string& a, const std::string& b) {
            const std::string la = lower(a);
            const std::string lb = lower(b);
            return la != lb ? la < lb : a < b;
        });
        return entries;
    }

    Result<AgentToolResult> format(const std::string& dir, const std::vector<std::string>& entries,
                                   std::int64_t limit) const {
        std::vector<std::string> results;
        bool limitReached = false;
        for (const std::string& entry : entries) {
            if (static_cast<std::int64_t>(results.size()) >= limit) {
                limitReached = true;
                break;
            }
            const auto info = m_fileSystem.stat(dir + "/" + entry);
            if (!info.has_value()) {
                continue;
            }
            results.push_back(entry + (info->isDirectory ? "/" : ""));
        }
        if (results.empty()) {
            return m_results.text("(empty directory)");
        }
        const TruncationResult truncation =
            m_truncator.truncateHead(m_results.join(results, "\n"), std::numeric_limits<std::int64_t>::max());
        std::string output = truncation.content;
        Json details = Json::object();
        std::vector<std::string> notices;
        if (limitReached) {
            notices.push_back(std::to_string(limit) + " entries limit reached. Use limit=" +
                              std::to_string(limit * 2) + " for more");
            details["entryLimitReached"] = limit;
        }
        if (truncation.truncated) {
            notices.push_back(m_truncator.formatSize(TextTruncator::kDefaultMaxBytes) + " limit reached");
            details["truncation"] = m_results.truncationJson(truncation);
        }
        if (!notices.empty()) {
            output += "\n\n[" + m_results.join(notices, ". ") + "]";
        }
        return m_results.text(output, details.empty() ? Json() : details);
    }

    IFileSystem& m_fileSystem;
    std::string m_cwd;
    PathResolver m_paths;
    TextTruncator m_truncator;
    ToolResultFactory m_results;
    Tool m_definition;
};
