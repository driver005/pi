export module pi.types.document_action;

import std;
export import pi.types.json;

/** What one commit does to one document: create it, change its content, retire it (any combination). */
export struct DocumentAction {
    std::optional<Json> create;
    std::optional<Json> content;
    bool retire = false;
};
