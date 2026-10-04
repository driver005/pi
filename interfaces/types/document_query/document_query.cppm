export module pi.types.document_query;

import std;
export import pi.types.document_point;
export import pi.types.json;

/** Ordered scan of the document incarnations alive in one exact scope at one point. */
export struct DocumentQuery {
    Json scope;
    DocumentPoint at;
    std::optional<std::string> kind;
};
