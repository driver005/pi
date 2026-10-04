export module pi.support.mcp_result_converter;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_crypto;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.support.text_truncator;
export import pi.types.agent_tool_result;
export import pi.types.json;
export import pi.types.mcp_call_result;

/**
 * Turns an MCP tools/call result into what the model sees: text and images. Text beyond 20KB keeps
 * its start and end with the middle cut out (Codex's format) and the full text goes to a private temp
 * file the model can read; non-image binary resources are saved to temp files too. Error results
 * keep their content. Port of convertMcpResult in packages/coding-agent/src/extensions/mcp/tools.ts
 * (no codemode, so `structuredContent` carries the result without `_meta` for programmatic callers).
 */
export class McpResultConverter {
public:
    static constexpr std::int64_t kOutputMaxBytes = 20 * 1024;

    McpResultConverter(IFileSystem& files, const ICrypto& crypto, const IBase64Codec& base64, const IEnvironment& environment)
        : m_files(files),
          m_crypto(crypto),
          m_base64(base64),
          m_environment(environment) {}

    AgentToolResult convert(const std::string& server, const std::string& tool, const McpCallResult& result) {
        std::vector<UserContentBlock> converted =
            result.content.is_array() && !result.content.empty() ? modelContent(result.content)
                                                                 : structuredFallback(result);
        if (result.isError && textOf(converted).empty()) {
            converted.push_back(text("MCP tool " + server + "/" + tool + " returned an error"));
        }
        std::optional<std::string> fullOutputPath;
        AgentToolResult out;
        out.content = limit(converted, fullOutputPath);
        out.details = Json{{"server", server}, {"tool", tool}};
        if (fullOutputPath) {
            out.details["fullOutputPath"] = *fullOutputPath;
        }
        out.structuredContent = scriptResult(result);
        out.isError = result.isError;
        return out;
    }

private:
    std::vector<UserContentBlock> modelContent(const Json& blocks) {
        std::vector<UserContentBlock> out;
        for (const Json& block : blocks) {
            for (UserContentBlock& converted : blockContent(block)) {
                out.push_back(std::move(converted));
            }
        }
        return out;
    }

    std::vector<UserContentBlock> blockContent(const Json& block) {
        if (str(block, "type") == "resource_link") {
            return resourceLink(block);
        }
        if (str(block, "type") == "resource" && block.contains("resource") && block["resource"].is_object()) {
            const Json& resource = block["resource"];
            if (resource.contains("blob") && resource["blob"].is_string() &&
                str(resource, "mimeType").rfind("image/", 0) != 0) {
                return binaryResource(resource);
            }
        }
        return plainContent(block);
    }

    std::vector<UserContentBlock> resourceLink(const Json& block) {
        std::vector<std::string> details;
        if (const auto mimeType = optionalStr(block, "mimeType"); mimeType && !mimeType->empty()) {
            details.push_back(*mimeType);
        }
        if (block.contains("size") && block["size"].is_number()) {
            details.push_back(m_truncator.formatSize(block["size"].get<std::int64_t>()));
        }
        std::string joined;
        for (std::size_t i = 0; i < details.size(); ++i) {
            joined += (i > 0 ? ", " : "") + details[i];
        }
        const std::string title = optionalStr(block, "title").value_or(str(block, "name"));
        std::string line = "[Resource " + str(block, "uri") + " \"" + title + "\"";
        if (!joined.empty()) {
            line += " (" + joined + ")";
        }
        if (const auto description = optionalStr(block, "description"); description && !description->empty()) {
            line += ": " + *description;
        }
        return {text(line + "]")};
    }

    std::vector<UserContentBlock> binaryResource(const Json& resource) {
        const std::string uri = str(resource, "uri");
        const std::string mimeType = str(resource, "mimeType");
        const auto data = m_base64.decode(str(resource, "blob"));
        if (!data) {
            return {text("[Binary resource " + uri + " could not be decoded]")};
        }
        if (textMimeType(mimeType)) {
            return {text(*data)};
        }
        const std::string kind = (mimeType.empty() ? std::string("unknown type") : mimeType) + ", " +
                                 m_truncator.formatSize(static_cast<std::int64_t>(data->size()));
        const auto path = saveToTempFile(*data, extensionOf(uri));
        if (!path) {
            return {text("[Binary resource " + uri + " (" + kind + ") could not be saved: " +
                         path.error().message + "]")};
        }
        return {text("[Binary resource " + uri + " (" + kind + ") saved to " + *path + "]")};
    }

    std::vector<UserContentBlock> plainContent(const Json& block) const {
        const std::string type = str(block, "type");
        if (type == "text") {
            return {text(str(block, "text"))};
        }
        if (type == "image") {
            return {image(str(block, "data"), str(block, "mimeType"))};
        }
        if (type == "audio") {
            return {text("[audio " + str(block, "mimeType") + " omitted]")};
        }
        if (type == "resource_link") {
            return {text(str(block, "name") + ": " + str(block, "uri"))};
        }
        if (type == "resource" && block.contains("resource") && block["resource"].is_object()) {
            const Json& resource = block["resource"];
            if (const auto body = optionalStr(resource, "text")) {
                return {text(*body)};
            }
            const std::string mimeType = str(resource, "mimeType");
            if (mimeType.rfind("image/", 0) == 0) {
                return {image(str(resource, "blob"), mimeType)};
            }
            return {text("[binary resource " + str(resource, "uri") + " (" +
                         (mimeType.empty() ? std::string("unknown type") : mimeType) + ") omitted]")};
        }
        return {text("[unsupported MCP content " + type + "]")};
    }

    std::vector<UserContentBlock> structuredFallback(const McpCallResult& result) const {
        if (result.structuredContent.is_null()) {
            return {};
        }
        return {text(result.structuredContent.dump(2, ' ', false, Json::error_handler_t::replace))};
    }

    std::vector<UserContentBlock> limit(const std::vector<UserContentBlock>& content, std::optional<std::string>& fullOutputPath) {
        const std::string combined = textOf(content);
        const MiddleTruncation truncation = m_truncator.truncateMiddle(combined, kOutputMaxBytes);
        if (!truncation.truncated) {
            return content;
        }
        std::string where;
        const auto saved = saveToTempFile(combined, ".txt");
        if (saved) {
            fullOutputPath = *saved;
            where = "[Full output: " + *saved + " (read it with offset/limit)]";
        } else {
            where = "[Could not save the full output: " + saved.error().message + "]";
        }
        const std::int64_t tokens = (truncation.totalBytes + 3) / 4;
        std::vector<UserContentBlock> out{text("Warning: truncated output (original token count: " +
                                               std::to_string(tokens) + ")\nTotal output lines: " +
                                               std::to_string(truncation.totalLines) + "\n\n" +
                                               truncation.content + "\n\n" + where)};
        for (const UserContentBlock& block : content) {
            if (std::holds_alternative<ImageContent>(block)) {
                out.push_back(block);
            }
        }
        return out;
    }

    Result<std::string> saveToTempFile(const std::string& data, const std::string& extension) {
        const std::string dir = m_environment.get("TMPDIR").value_or("/tmp");
        const std::string path = (dir.ends_with('/') ? dir : dir + "/") + "pi-mcp-" +
                                 hex(m_crypto.randomBytes(8)) + extension;
        // Results can carry private data, so only the user may read the file.
        auto written = m_files.writeFilePrivate(path, data);
        if (!written) {
            return std::unexpected(written.error());
        }
        return path;
    }

    std::string textOf(const std::vector<UserContentBlock>& content) const {
        std::string out;
        bool first = true;
        for (const UserContentBlock& block : content) {
            if (const auto* textBlock = std::get_if<TextContent>(&block)) {
                out += (first ? "" : "\n") + textBlock->text;
                first = false;
            }
        }
        return out;
    }

    UserContentBlock text(const std::string& value) const {
        TextContent block;
        block.text = value;
        return block;
    }

    UserContentBlock image(const std::string& data, const std::string& mimeType) const {
        ImageContent block;
        block.data = data;
        block.mimeType = mimeType;
        return block;
    }

    std::string str(const Json& object, const std::string& key) const {
        return optionalStr(object, key).value_or("");
    }

    std::optional<std::string> optionalStr(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return std::nullopt;
    }

    bool textMimeType(const std::string& mimeType) const {
        std::string type = mimeType.substr(0, mimeType.find(';'));
        const std::size_t first = type.find_first_not_of(" \t");
        type = first == std::string::npos ? "" : type.substr(first, type.find_last_not_of(" \t") - first + 1);
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto endsWith = [&type](const std::string& suffix) {
            return type.size() >= suffix.size() && type.compare(type.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return type.rfind("text/", 0) == 0 || type == "application/json" || endsWith("+json") ||
               endsWith("+xml");
    }

    std::string extensionOf(const std::string& uri) const {
        std::string path = uri.substr(0, uri.find_first_of("?#"));
        const std::size_t scheme = path.find("://");
        if (scheme != std::string::npos) {
            const std::size_t slash = path.find('/', scheme + 3);
            path = slash == std::string::npos ? "" : path.substr(slash);
        }
        const std::size_t dot = path.rfind('.');
        if (dot == std::string::npos || path.find('/', dot) != std::string::npos) {
            return ".bin";
        }
        const std::string extension = path.substr(dot);
        const bool valid = extension.size() >= 2 && extension.size() <= 9 &&
                           std::all_of(extension.begin() + 1, extension.end(),
                                       [](unsigned char c) { return std::isalnum(c) != 0; });
        return valid ? extension : ".bin";
    }

    std::string hex(const std::string& bytes) const {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string out;
        for (const char c : bytes) {
            out.push_back(digits[static_cast<unsigned char>(c) >> 4]);
            out.push_back(digits[static_cast<unsigned char>(c) & 0x0F]);
        }
        return out;
    }

    Json scriptResult(const McpCallResult& result) const {
        Json out = Json{{"content", result.content}};
        if (!result.structuredContent.is_null()) {
            out["structuredContent"] = result.structuredContent;
        }
        if (result.isError) {
            out["isError"] = true;
        }
        return out;
    }

    IFileSystem& m_files;
    const ICrypto& m_crypto;
    const IBase64Codec& m_base64;
    const IEnvironment& m_environment;
    TextTruncator m_truncator;
};
