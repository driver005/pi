export module pi.durable.i_transaction_host;

import std;
export import pi.durable.i_storage;
export import pi.types.doc_definition;
export import pi.types.document_address;
export import pi.types.json;
export import pi.types.loaded_document;
export import pi.types.result;

/** The session services a transaction uses while it holds the mutation line. */
export class ITransactionHost {
public:
    virtual ~ITransactionHost() = default;

    virtual IStorage& storage() = 0;
    /** The cached current incarnation of an address, without loading. */
    virtual std::shared_ptr<LoadedDocument> cached(const std::string& addressId) = 0;
    /** The cached or freshly loaded (and migrated) current incarnation; null when the address is empty. */
    virtual Result<std::shared_ptr<LoadedDocument>> load(const DocDefinition& definition, const std::string& addressId,
                                                         const DocumentAddress& address) = 0;
    virtual void install(std::shared_ptr<LoadedDocument> document) = 0;
    /** Forgets a retired incarnation if it still occupies its address. */
    virtual void evict(const std::string& addressId, std::int64_t recordId) = 0;
};
