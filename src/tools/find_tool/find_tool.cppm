module;
#include <nlohmann/json.hpp>

export module pi.tools.find_tool;

import std;
import pi.platform.i_file_system;
import pi.support.gitignore_matcher;
import pi.support.glob_matcher;
import pi.support.path_resolver;
import pi.support.text_truncator;
import pi.support.tool_result_factory;
export import pi.tool.i_tool;

/**
 * The `find` tool: glob search that respects .gitignore/.ignore/.fdignore, includes hidden files and
 * skips .git. Where the TypeScript tool shells out to `fd`, this walks the tree itself, so results
 * are deterministic (sorted) and no external binary is needed. Patterns containing '/' match the
 * path relative to the search root (an implicit leading `**` /), other patterns match basenames.
 */
export class FindTool : public ITool {
public:
    static constexpr std::int64_t kDefaultLimit = 1000;

    FindTool(IFileSystem& fileSystem, std::string cwd)
        : m_fileSystem(fileSystem), m_cwd(std::move(cwd)), m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "find";
        m_definition.description =
            "Search for files by glob pattern. Returns matching file paths relative to the search directory. "
            "Respects .gitignore. Output is truncated to 1000 results or 50KB (whichever is hit first).";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "pattern":{"type":"string","description":"Glob pattern to match files, e.g. '*.ts', '**/*.json', or 'src/**/*.spec.ts'"},
                "path":{"type":"string","description":"Directory to search in (default: current directory)"},
                "limit":{"type":"number","description":"Maximum number of results (default: 1000)"}
            },
            "required":["pattern"]})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "find";
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
        const std::string root = m_paths.resolveToCwd(requested.empty() ? "." : requested, m_cwd);
        if (!m_fileSystem.exists(root)) {
            return std::unexpected(Error{"not_found", "Path not found: " + root});
        }
        const std::int64_t limit = params.contains("limit") && params["limit"].is_number()
                                       ? static_cast<std::int64_t>(params["limit"].get<double>())
                                       : kDefaultLimit;
        const std::string pattern = params["pattern"].get<std::string>();
        std::vector<std::pair<std::string, GitignoreMatcher>> matchers = ancestorMatchers(root);
        std::vector<std::string> results;
        std::set<std::string> visited;
        walk(root, root, pattern, limit, signal, matchers, visited, results);
        if (signal != nullptr && signal->aborted()) {
            return std::unexpected(Error{"aborted", "Operation aborted"});
        }
        if (results.empty()) {
            return m_results.text("No files found matching pattern");
        }
        return format(results, limit);
    }

private:
    std::string readIgnoreFile(const std::string& dir) const {
        std::string rules;
        for (const char* name : {".gitignore", ".ignore", ".fdignore"}) {
            auto content = m_fileSystem.readFile(dir + "/" + name);
            if (content.has_value()) {
                rules += *content + "\n";
            }
        }
        return rules;
    }

    std::vector<std::pair<std::string, GitignoreMatcher>> ancestorMatchers(const std::string& root) const {
        std::vector<std::pair<std::string, GitignoreMatcher>> matchers;
        std::vector<std::string> chain;
        std::filesystem::path current = std::filesystem::path(root).parent_path();
        while (!current.empty() && current != current.root_path()) {
            chain.push_back(current.string());
            if (m_fileSystem.exists(current.string() + "/.git")) {
                break;
            }
            current = current.parent_path();
        }
        const bool inRepo = !chain.empty() && m_fileSystem.exists(chain.back() + "/.git");
        if (inRepo) {
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                matchers.emplace_back(*it, GitignoreMatcher(readIgnoreFile(*it)));
            }
        }
        return matchers;
    }

    bool isIgnored(const std::string& absolute, bool isDirectory,
                   const std::vector<std::pair<std::string, GitignoreMatcher>>& matchers) const {
        for (const auto& [dir, matcher] : matchers) {
            const std::string relative = std::filesystem::path(absolute).lexically_relative(dir).generic_string();
            if (!relative.empty() && relative.rfind("..", 0) != 0 && matcher.ignores(relative, isDirectory)) {
                return true;
            }
        }
        return false;
    }

    bool matches(const std::string& pattern, const std::string& relative, const std::string& name) const {
        if (pattern.find('/') == std::string::npos) {
            return m_glob.matches(pattern, name);
        }
        std::string effective = pattern;
        if (!effective.empty() && effective[0] == '/') {
            effective.erase(0, 1);
        } else if (effective.rfind("**/", 0) != 0 && effective != "**") {
            effective = "**/" + effective;
        }
        return m_glob.matches(effective, relative);
    }

    void walk(const std::string& root, const std::string& dir, const std::string& pattern, std::int64_t limit,
              const std::shared_ptr<AbortSignal>& signal,
              std::vector<std::pair<std::string, GitignoreMatcher>> matchers, std::set<std::string>& visited,
              std::vector<std::string>& results) const {
        if (!visited.insert(m_fileSystem.realPath(dir)).second) {
            return;
        }
        if (const std::string rules = readIgnoreFile(dir); !rules.empty()) {
            matchers.emplace_back(dir, GitignoreMatcher(rules));
        }
        auto entries = m_fileSystem.listDirectory(dir);
        if (!entries.has_value()) {
            return;
        }
        std::sort(entries->begin(), entries->end());
        for (const std::string& name : *entries) {
            if (static_cast<std::int64_t>(results.size()) >= limit || (signal != nullptr && signal->aborted())) {
                return;
            }
            if (name == ".git") {
                continue;
            }
            const std::string absolute = dir + "/" + name;
            const auto info = m_fileSystem.stat(absolute);
            if (!info.has_value()) {
                continue;
            }
            if (isIgnored(absolute, info->isDirectory, matchers)) {
                continue;
            }
            const std::string relative = std::filesystem::path(absolute).lexically_relative(root).generic_string();
            if (matches(pattern, relative, name)) {
                results.push_back(relative + (info->isDirectory ? "/" : ""));
            }
            if (info->isDirectory) {
                walk(root, absolute, pattern, limit, signal, matchers, visited, results);
            }
        }
    }

    Result<AgentToolResult> format(const std::vector<std::string>& results, std::int64_t limit) const {
        const bool limitReached = static_cast<std::int64_t>(results.size()) >= limit;
        const TruncationResult truncation =
            m_truncator.truncateHead(m_results.join(results, "\n"), std::numeric_limits<std::int64_t>::max());
        std::string output = truncation.content;
        Json details = Json::object();
        std::vector<std::string> notices;
        if (limitReached) {
            notices.push_back(std::to_string(limit) + " results limit reached. Use limit=" + std::to_string(limit * 2) +
                              " for more, or refine pattern");
            details["resultLimitReached"] = limit;
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
    GlobMatcher m_glob;
    TextTruncator m_truncator;
    ToolResultFactory m_results;
    Tool m_definition;
};
