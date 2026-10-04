module;

#include <cstdint>

export module pi.types.document_plan;

import std;
export import pi.types.doc_definition;
export import pi.types.document_entry;
export import pi.types.json;

/** What one staged incarnation writes and publishes, decided before storage admission. */
export struct DocumentPlan {
    std::string addressId;
    /** A document record (already committed) or a create record (new). */
    Json record;
    bool committed = false;
    bool retire = false;
    /** The content write (`document.create`, `document.copy`, `document.change`), or null. */
    Json content;
    /** The entry whose change this plan carries; null for copies and retirement-only plans. */
    DocumentEntry* change = nullptr;
    std::optional<std::int64_t> conversationId;
};
