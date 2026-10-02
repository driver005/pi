export module pi.support.tool_result_factory;

import std;
export import pi.types.agent_tool_result;
export import pi.types.json;
export import pi.types.truncation_result;

/** Builders shared by the built-in tools for results, truncation details and error texts. */
export class ToolResultFactory {
public:
    /** A result with one text block; `details` null means none. */
    AgentToolResult text(const std::string& text, Json details = Json()) const {
        AgentToolResult result;
        result.content.emplace_back(TextContent{text, std::nullopt});
        result.details = std::move(details);
        return result;
    }

    Json truncationJson(const TruncationResult& truncation) const {
        Json json = {{"content", truncation.content},
                     {"truncated", truncation.truncated},
                     {"truncatedBy", truncation.truncatedBy.empty() ? Json(nullptr) : Json(truncation.truncatedBy)},
                     {"totalLines", truncation.totalLines},
                     {"totalBytes", truncation.totalBytes},
                     {"outputLines", truncation.outputLines},
                     {"outputBytes", truncation.outputBytes},
                     {"lastLinePartial", truncation.lastLinePartial},
                     {"firstLineExceedsLimit", truncation.firstLineExceedsLimit},
                     {"maxLines", truncation.maxLines},
                     {"maxBytes", truncation.maxBytes}};
        return json;
    }

    /** Node-style message of a failed fs.access: "ENOENT: no such file or directory, access 'p'". */
    std::string accessError(const std::string& path, bool exists) const {
        return exists ? "EACCES: permission denied, access '" + path + "'"
                      : "ENOENT: no such file or directory, access '" + path + "'";
    }

    /** Splits like JavaScript's String.split("\n"): "a\nb\n" gives {"a","b",""}. */
    std::vector<std::string> splitLines(const std::string& content) const {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (true) {
            const std::size_t end = content.find('\n', start);
            if (end == std::string::npos) {
                lines.push_back(content.substr(start));
                return lines;
            }
            lines.push_back(content.substr(start, end - start));
            start = end + 1;
        }
    }

    std::string join(const std::vector<std::string>& parts, const std::string& separator) const {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            out += (i > 0 ? separator : "") + parts[i];
        }
        return out;
    }
};
