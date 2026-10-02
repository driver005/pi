module;

#include <nlohmann/json.hpp>

export module pi.types.frontmatter_document;

import std;
export import pi.types.json;

/** A markdown file split into its YAML header (as JSON object) and body text. */
export struct FrontmatterDocument {
    Json frontmatter = Json::object();
    std::string body;
};
