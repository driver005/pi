module;

#include <cstdint>

export module pi.support.fork_planner;

import std;
export import pi.durable.i_storage;
export import pi.support.document_resolver;
export import pi.types.fork_copy;
export import pi.types.json;
export import pi.types.result;

/**
 * Selects every persisted conversation document a fork copies into the child conversation. Documents
 * at the fork entry's own conversation follow their "asOf" policy at the entry's commit; documents of
 * the parent follow their "current" policy at the present. Port of session/forks.ts.
 */
export class ForkPlanner {
public:
    static constexpr std::size_t kScanPageSize = 256;

    Result<std::vector<ForkCopy>> prepare(IStorage& storage, std::int64_t parentConversationId, std::int64_t at,
                                          std::int64_t childConversationId) const {
        auto entry = storage.visibleEntry(parentConversationId, at);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (!*entry) {
            return std::unexpected(Error{"durable_error", "Entry " + std::to_string(at) +
                                                              " is not visible from conversation " +
                                                              std::to_string(parentConversationId)});
        }
        std::vector<ForkCopy> copies;
        std::set<std::string> copied;
        const Json entryScope = Json::object(
            {{"kind", "conversation"}, {"conversationId", (*entry)->entry.at("conversationId").get<std::int64_t>()}});
        DocumentPoint asOf{false, (*entry)->commitSeq};
        if (auto collected = collect(storage, entryScope, asOf, "asOf", childConversationId, copies, copied);
            !collected) {
            return std::unexpected(collected.error());
        }
        const Json parentScope =
            Json::object({{"kind", "conversation"}, {"conversationId", parentConversationId}});
        if (auto collected = collect(storage, parentScope, DocumentPoint{true, 0}, "current", childConversationId,
                                     copies, copied);
            !collected) {
            return std::unexpected(collected.error());
        }
        return copies;
    }

private:
    Result<void> collect(IStorage& storage, const Json& scope, const DocumentPoint& at, const std::string& policy,
                         std::int64_t childConversationId, std::vector<ForkCopy>& copies,
                         std::set<std::string>& copied) const {
        std::optional<Json> cursor;
        do {
            auto page = storage.scanDocuments(DocumentQuery{scope, at, std::nullopt}, kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const Json& source : page->items) {
                if (source.at("scope").at("kind") != "conversation" || source.value("fork", std::string()) != policy) {
                    continue;
                }
                if (auto added = addCopy(storage, source, at, childConversationId, copies, copied); !added) {
                    return added;
                }
            }
            cursor = page->next;
        } while (cursor);
        return {};
    }

    Result<void> addCopy(IStorage& storage, const Json& source, const DocumentPoint& at,
                         std::int64_t childConversationId, std::vector<ForkCopy>& copies,
                         std::set<std::string>& copied) const {
        auto id = storage.mintId();
        if (!id) {
            return std::unexpected(id.error());
        }
        Json record = Json::object({{"id", *id}, {"kind", source.at("kind")}});
        if (source.contains("key")) {
            record["key"] = source.at("key");
        }
        record["scope"] = Json::object({{"kind", "conversation"}, {"conversationId", childConversationId}});
        record["history"] = source.at("history");
        record["fork"] = source.at("fork");
        if (!copied.insert(m_resolver.recordAddressId(record)).second) {
            const std::string member = record.contains("key")
                                           ? record.at("kind").get<std::string>() + "/" + record.at("key").get<std::string>()
                                           : record.at("kind").get<std::string>();
            return std::unexpected(Error{"durable_error", "Fork selects multiple source documents for " + member});
        }
        Json from = Json::object({{"id", source.at("id")}});
        from["at"] = at.current ? Json("current") : Json(at.seq);
        copies.push_back(ForkCopy{std::move(record), std::move(from)});
        return {};
    }

    DocumentResolver m_resolver;
};
