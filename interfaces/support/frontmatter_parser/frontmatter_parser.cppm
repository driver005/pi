module;

#include "third_party/yaml_bridge/yaml_bridge.h"

export module pi.support.frontmatter_parser;

import std;
export import pi.types.frontmatter_document;
export import pi.types.json;
export import pi.types.result;

/**
 * Splits `---` delimited YAML frontmatter from markdown (BOM and CRLF tolerated).
 * YAML parsing runs in third_party/yaml_bridge, which owns the exception boundary.
 */
export class FrontmatterParser {
public:
    /** Documents without a header yield an empty object and the whole text as body. */
    Result<FrontmatterDocument> parse(std::string_view text) const {
        const std::string normalized = normalize(text);
        FrontmatterDocument document;
        if (!normalized.starts_with("---\n")) {
            document.body = normalized;
            return document;
        }
        std::size_t close = normalized.find("\n---", 3);
        while (close != std::string::npos) {
            const std::size_t after = close + 4;
            if (after >= normalized.size() || normalized[after] == '\n') {
                break;
            }
            close = normalized.find("\n---", close + 1);
        }
        if (close == std::string::npos) {
            document.body = normalized;
            return document;
        }
        const std::string yamlText = normalized.substr(4, close - 4 + 1);
        const std::size_t bodyStart = close + 4 < normalized.size() ? close + 5 : normalized.size();
        document.body = normalized.substr(bodyStart);
        std::string jsonText;
        std::string failure;
        if (!yamlToJson(yamlText, &jsonText, &failure)) {
            return std::unexpected(Error{"frontmatter_invalid", failure});
        }
        Json value = Json::parse(jsonText, nullptr, false);
        document.frontmatter = value.is_object() ? std::move(value) : Json::object();
        return document;
    }

private:
    std::string normalize(std::string_view text) const {
        std::string out;
        out.reserve(text.size());
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') {
                out.push_back('\n');
                i += (i + 1 < text.size() && text[i + 1] == '\n') ? 1 : 0;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }
};
