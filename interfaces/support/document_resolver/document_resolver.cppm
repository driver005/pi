module;

#include <cstdint>

export module pi.support.document_resolver;

import std;
export import pi.types.doc_address_args;
export import pi.types.doc_definition;
export import pi.types.document_address;
export import pi.types.json;
export import pi.types.resolved_address;
export import pi.types.result;

/**
 * Resolves the logical address of a document and checks stored records against a definition.
 * Port of packages/durable/src/documents.ts (address, create record, scope/version checks).
 */
export class DocumentResolver {
public:
    /** Builds the address a definition and its arguments name; fails when the owner or key is missing. */
    Result<ResolvedAddress> resolve(const DocDefinition& definition, const DocAddressArgs& args) const {
        Json scope = Json::object({{"kind", definition.scope}});
        if (definition.scope == "conversation" || definition.scope == "task") {
            if (!args.owner) {
                return std::unexpected(Error{"type_error", "Document " + definition.kind + " requires a " +
                                                               definition.scope + " ID"});
            }
            scope[definition.scope == "task" ? "taskId" : "conversationId"] = *args.owner;
        }
        DocumentAddress address{definition.kind, scope, std::nullopt};
        if (definition.family) {
            if (!args.key) {
                return std::unexpected(Error{"type_error", "Document " + definition.kind + " requires a key"});
            }
            address.key = args.key;
        }
        std::string id = addressId(address);
        return ResolvedAddress{std::move(address), std::move(id)};
    }

    /** Stable string identity of one logical address. */
    std::string addressId(const DocumentAddress& address) const {
        return identity(address.kind, address.scope, address.key ? Json(*address.key) : Json(nullptr));
    }

    /** The identity of a stored record or create record (kind, scope and key fields). */
    std::string recordAddressId(const Json& record) const {
        Json key = record.contains("key") ? record.at("key") : Json(nullptr);
        return identity(record.at("kind").get<std::string>(), record.at("scope"), key);
    }

    /** The address of a stored record or create record. */
    DocumentAddress recordAddress(const Json& record) const {
        DocumentAddress address{record.at("kind").get<std::string>(), record.at("scope"), std::nullopt};
        if (record.contains("key")) {
            address.key = record.at("key").get<std::string>();
        }
        return address;
    }

    /** The storage create record of a new incarnation at an address. */
    Json createRecord(const DocDefinition& definition, const DocumentAddress& address, std::int64_t id) const {
        Json record = Json::object({{"id", id}, {"kind", address.kind}});
        if (address.key) {
            record["key"] = *address.key;
        }
        record["scope"] = address.scope;
        if (address.scope.at("kind") == "conversation") {
            record["history"] = definition.history;
            record["fork"] = definition.fork;
        }
        return record;
    }

    /** Rejects access whose definition disagrees with the persisted scope, history or fork semantics. */
    Result<void> checkScope(const DocDefinition& definition, const Json& record) const {
        const std::string scope = record.at("scope").at("kind").get<std::string>();
        bool mismatch = scope != definition.scope;
        if (!mismatch && scope == "conversation") {
            mismatch = record.value("history", std::string()) != definition.history ||
                       record.value("fork", std::string()) != definition.fork;
        }
        if (mismatch) {
            return std::unexpected(Error{"type_error", "Document " + std::to_string(record.at("id").get<std::int64_t>()) +
                                                           " (" + record.at("kind").get<std::string>() +
                                                           ") does not match the supplied definition semantics"});
        }
        return {};
    }

    /** Rejects access to a stored version the definition cannot use. */
    Result<void> checkVersion(const DocDefinition& definition, const Json& record, std::int64_t version) const {
        const std::string label = "Document " + std::to_string(record.at("id").get<std::int64_t>()) + " (" +
                                  record.at("kind").get<std::string>() + ")";
        if (version > definition.version) {
            return std::unexpected(Error{"document_version", label + " has newer version " + std::to_string(version) +
                                                                 " than " + std::to_string(definition.version)});
        }
        if (version < definition.version && !definition.migrate) {
            return std::unexpected(
                Error{"document_version", label + " requires migration from version " + std::to_string(version)});
        }
        return {};
    }

    /** Validates a stored value and migrates it to the definition's version. */
    Result<Json> materialize(const DocDefinition& definition, const Json& record, std::int64_t version,
                             const Json& value) const {
        if (auto scoped = checkScope(definition, record); !scoped) {
            return std::unexpected(scoped.error());
        }
        if (auto versioned = checkVersion(definition, record, version); !versioned) {
            return std::unexpected(versioned.error());
        }
        if (version == definition.version) {
            return value;
        }
        return definition.migrate(value, version);
    }

private:
    std::string identity(const std::string& kind, const Json& scope, const Json& key) const {
        const std::string scopeKind = scope.at("kind").get<std::string>();
        Json owner = nullptr;
        if (scopeKind == "conversation") {
            owner = scope.at("conversationId");
        } else if (scopeKind == "task") {
            owner = scope.at("taskId");
        }
        return Json::array({kind, scopeKind, owner, key}).dump();
    }
};
