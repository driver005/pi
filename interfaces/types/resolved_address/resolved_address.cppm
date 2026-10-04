export module pi.types.resolved_address;

import std;
export import pi.types.document_address;

/** A document's logical address together with its string identity for maps. */
export struct ResolvedAddress {
    DocumentAddress address;
    std::string id;
};
