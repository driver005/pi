#include "interfaces/support/frontmatter_parser/frontmatter_parser.h"

#include <cstdlib>

#include <yaml-cpp/yaml.h>

std::string FrontmatterParser::normalize(std::string_view text) const {
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

Result<FrontmatterDocument> FrontmatterParser::parse(std::string_view text) const {
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
    try {
        const YAML::Node node = YAML::Load(yamlText);
        Json value = nodeToJson(node);
        document.frontmatter = value.is_object() ? std::move(value) : Json::object();
    } catch (const YAML::Exception& error) {
        return std::unexpected(Error{"frontmatter_invalid", error.what()});
    }
    return document;
}

Json FrontmatterParser::nodeToJson(const YAML::Node& node) const {
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            Json object = Json::object();
            for (const auto& entry : node) {
                object[entry.first.as<std::string>()] = nodeToJson(entry.second);
            }
            return object;
        }
        case YAML::NodeType::Sequence: {
            Json array = Json::array();
            for (const auto& entry : node) {
                array.push_back(nodeToJson(entry));
            }
            return array;
        }
        case YAML::NodeType::Scalar:
            return scalarToJson(node);
        default:
            return nullptr;
    }
}

Json FrontmatterParser::scalarToJson(const YAML::Node& node) const {
    const std::string text = node.Scalar();
    if (node.Tag() == "!") {
        return text;
    }
    if (text == "true" || text == "false") {
        return text == "true";
    }
    if (text == "null" || text == "~" || text.empty()) {
        return text.empty() ? Json("") : Json(nullptr);
    }
    char* end = nullptr;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0') {
        return integer;
    }
    const double real = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && *end == '\0') {
        return real;
    }
    return text;
}
