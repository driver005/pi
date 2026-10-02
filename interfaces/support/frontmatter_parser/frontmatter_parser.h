#pragma once

#include <string>
#include <string_view>

#include "interfaces/types/frontmatter_document/frontmatter_document.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/result/result.h"

namespace YAML {
class Node;
}

/**
 * Splits `---` delimited YAML frontmatter from markdown (BOM and CRLF tolerated).
 * This module is compiled with exceptions enabled: yaml-cpp throws on malformed input and the
 * exception never leaves parse(), which returns an Error instead.
 */
class FrontmatterParser {
public:
    /** Documents without a header yield an empty object and the whole text as body. */
    Result<FrontmatterDocument> parse(std::string_view text) const;

private:
    std::string normalize(std::string_view text) const;
    Json nodeToJson(const YAML::Node& node) const;
    Json scalarToJson(const YAML::Node& node) const;
};
