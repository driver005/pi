module;
#include <nlohmann/json.hpp>

export module pi.tools.read_tool;

import std;
import pi.platform.i_base64_codec;
import pi.platform.i_file_system;
import pi.support.path_resolver;
import pi.support.text_truncator;
import pi.support.tool_result_factory;
export import pi.tool.i_tool;

/**
 * The `read` tool (port of core/tools/read.ts): text files with offset/limit and head truncation,
 * and images (png, jpeg, gif, webp, bmp) returned as attachments. Unlike the TypeScript tool,
 * images are not resized: ones whose base64 form exceeds 4.5MB are reported instead.
 */
export class ReadTool : public ITool {
public:
    static constexpr std::size_t kMaxImageBase64Bytes = 4'718'592;

    ReadTool(IFileSystem& fileSystem, IBase64Codec& base64, std::string cwd)
        : m_fileSystem(fileSystem),
          m_base64(base64),
          m_cwd(std::move(cwd)),
          m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "read";
        m_definition.description =
            "Read the contents of a file. Supports text files and images (jpg, png, gif, webp, bmp). "
            "Images are sent as attachments. For text files, output is truncated to 2000 lines or 50KB "
            "(whichever is hit first). Use offset/limit for large files. When you need the full file, "
            "continue with offset until complete.";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Path to the file to read (relative or absolute)"},
                "offset":{"type":"number","description":"Line number to start reading from (1-indexed)"},
                "limit":{"type":"number","description":"Maximum number of lines to read"}
            },
            "required":["path"]})json");
        m_definition.constrainedSampling = Json::parse(R"json({"type":"json_schema","strict":"prefer"})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "read";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    std::string promptSnippet() const override {
        return "Read file contents";
    }
    std::vector<std::string> promptGuidelines() const override {
        return {
            "Use read to examine files instead of cat or sed."};
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
        const std::string path = params["path"].get<std::string>();
        const std::string absolute = resolveReadPath(path);
        if (!m_fileSystem.isReadable(absolute)) {
            return std::unexpected(
                Error{"access", m_results.accessError(absolute, m_fileSystem.exists(absolute))});
        }
        auto content = m_fileSystem.readFile(absolute);
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        if (signal != nullptr && signal->aborted()) {
            return std::unexpected(Error{"aborted", "Operation aborted"});
        }
        if (const std::string mime = detectImageMimeType(*content); !mime.empty()) {
            return imageResult(*content, mime);
        }
        return textResult(*content, path, params);
    }

private:
    std::string resolveReadPath(const std::string& path) const {
        const std::string resolved = m_paths.resolveToCwd(path, m_cwd);
        if (m_fileSystem.exists(resolved)) {
            return resolved;
        }
        for (const std::string& variant : m_paths.readPathVariants(resolved)) {
            if (m_fileSystem.exists(variant)) {
                return variant;
            }
        }
        return resolved;
    }

    std::string detectImageMimeType(const std::string& bytes) const {
        const auto starts = [&](std::string_view prefix) { return bytes.compare(0, prefix.size(), prefix) == 0; };
        if (starts("\x89PNG\r\n\x1a\n")) {
            return "image/png";
        }
        if (starts("\xFF\xD8\xFF")) {
            return "image/jpeg";
        }
        if (starts("GIF87a") || starts("GIF89a")) {
            return "image/gif";
        }
        if (bytes.size() >= 12 && starts("RIFF") && bytes.compare(8, 4, "WEBP") == 0) {
            return "image/webp";
        }
        if (starts("BM") && bytes.size() > 14) {
            return "image/bmp";
        }
        return "";
    }

    Result<AgentToolResult> imageResult(const std::string& bytes, const std::string& mime) const {
        const std::string encoded = m_base64.encode(bytes);
        AgentToolResult result;
        if (encoded.size() > kMaxImageBase64Bytes) {
            result.content.emplace_back(TextContent{
                "Read image file [" + mime + "]\nImage is too large to attach (" +
                    m_truncator.formatSize(static_cast<std::int64_t>(encoded.size())) + " base64, limit " +
                    m_truncator.formatSize(static_cast<std::int64_t>(kMaxImageBase64Bytes)) + ").",
                std::nullopt});
            return result;
        }
        result.content.emplace_back(TextContent{"Read image file [" + mime + "]", std::nullopt});
        result.content.emplace_back(ImageContent{encoded, mime});
        return result;
    }

    Result<AgentToolResult> textResult(const std::string& content, const std::string& path,
                                       const Json& params) const {
        const std::vector<std::string> allLines = m_results.splitLines(content);
        const auto totalLines = static_cast<std::int64_t>(allLines.size());
        const std::int64_t offset = params.contains("offset") && params["offset"].is_number()
                                        ? static_cast<std::int64_t>(params["offset"].get<double>())
                                        : 0;
        const std::int64_t startLine = offset > 0 ? std::max<std::int64_t>(0, offset - 1) : 0;
        const std::int64_t startDisplay = startLine + 1;
        if (startLine >= totalLines) {
            return std::unexpected(Error{"offset", "Offset " + std::to_string(offset) +
                                                       " is beyond end of file (" + std::to_string(totalLines) +
                                                       " lines total)"});
        }
        std::int64_t endLine = totalLines;
        std::optional<std::int64_t> userLimited;
        if (params.contains("limit") && params["limit"].is_number()) {
            endLine = std::min<std::int64_t>(startLine + static_cast<std::int64_t>(params["limit"].get<double>()),
                                             totalLines);
            userLimited = endLine - startLine;
        }
        const std::string selected = m_results.join(
            std::vector<std::string>(allLines.begin() + startLine, allLines.begin() + endLine), "\n");
        const TruncationResult truncation = m_truncator.truncateHead(selected);
        const std::string limit = m_truncator.formatSize(TextTruncator::kDefaultMaxBytes);
        if (truncation.firstLineExceedsLimit) {
            const std::string size = m_truncator.formatSize(static_cast<std::int64_t>(allLines[startLine].size()));
            return m_results.text("[Line " + std::to_string(startDisplay) + " is " + size + ", exceeds " + limit +
                                      " limit. Use bash: sed -n '" + std::to_string(startDisplay) + "p' " + path +
                                      " | head -c " + std::to_string(TextTruncator::kDefaultMaxBytes) + "]",
                                  Json{{"truncation", m_results.truncationJson(truncation)}});
        }
        if (truncation.truncated) {
            const std::int64_t endDisplay = startDisplay + truncation.outputLines - 1;
            std::string text = truncation.content + "\n\n[Showing lines " + std::to_string(startDisplay) + "-" +
                               std::to_string(endDisplay) + " of " + std::to_string(totalLines);
            text += truncation.truncatedBy == "lines" ? ". Use offset=" : " (" + limit + " limit). Use offset=";
            text += std::to_string(endDisplay + 1) + " to continue.]";
            return m_results.text(text, Json{{"truncation", m_results.truncationJson(truncation)}});
        }
        if (userLimited.has_value() && startLine + *userLimited < totalLines) {
            const std::int64_t remaining = totalLines - (startLine + *userLimited);
            return m_results.text(truncation.content + "\n\n[" + std::to_string(remaining) +
                                  " more lines in file. Use offset=" + std::to_string(startLine + *userLimited + 1) +
                                  " to continue.]");
        }
        return m_results.text(truncation.content);
    }

    IFileSystem& m_fileSystem;
    IBase64Codec& m_base64;
    std::string m_cwd;
    PathResolver m_paths;
    TextTruncator m_truncator;
    ToolResultFactory m_results;
    Tool m_definition;
};
