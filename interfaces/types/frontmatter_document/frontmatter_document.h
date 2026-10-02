#pragma once

#include <string>

#include "interfaces/types/json/json.h"

/** A markdown file split into its YAML header (as JSON object) and body text. */
struct FrontmatterDocument {
    Json frontmatter = Json::object();
    std::string body;
};
