module;

#include <cstdint>

export module pi.types.document_entry;

import std;
export import pi.types.doc_definition;
export import pi.types.document_address;
export import pi.types.json;
export import pi.types.loaded_document;

/** One document incarnation a transaction acquired, created, copied or retired. */
export struct DocumentEntry {
    std::string addressId;
    DocumentAddress address;
    /** Absent for definition-free fork copies and retirements found by a terminal-task scan. */
    std::optional<DocDefinition> definition;
    /** "none", "loaded", "created", "fork-copy" or "retire-only". */
    std::string target = "none";
    std::shared_ptr<LoadedDocument> loaded;
    /** created, fork-copy, retire-only: the record; fork-copy: also `source`. */
    Json record;
    Json source;
    std::int64_t version = 0;
    /** The value as acquired (diff base) and the value being edited. */
    Json before;
    Json draft;
    bool drafted = false;
    bool retireOnCommit = false;
    /** Filled by prepare: the committed-change operations. */
    Json ops = Json::array();
};
