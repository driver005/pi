module;

#include <cstdint>

export module pi.support.inbox_boundary;

import std;
export import pi.support.builtin_documents;
export import pi.support.entry_kinds;
export import pi.support.transaction;
export import pi.types.boundary;
export import pi.types.boundary_result;
export import pi.types.json;
export import pi.types.result;

/**
 * The queue of a conversation's submissions waiting for a boundary (`pi.inbox`), and the boundary rules that place
 * them (spec §6). Items are `{id, mode: "steer"|"followUp", content}` or `{id, mode: "write", entry}`. Port of
 * packages/durable/src/harness/inbox.ts.
 */
export class InboxBoundary {
public:
    /**
     * Reads what a boundary needs. Table reads must precede the commit's first table write, so callers prepare the
     * boundary at the start of their commit.
     */
    Result<Boundary> prepare(Transaction& tx, std::int64_t conversationId, const std::string& steeringMode,
                             const std::string& followUpMode) const {
        auto marker = tx.latestHeadMarker(conversationId);
        if (!marker) {
            return std::unexpected(marker.error());
        }
        auto inbox = tx.doc(m_documents.inbox(), owner(conversationId));
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        Boundary boundary;
        boundary.conversationId = conversationId;
        boundary.inbox = *inbox;
        boundary.steeringMode = steeringMode;
        boundary.followUpMode = followUpMode;
        if (*marker && (*marker)->contains("head")) {
            boundary.head = (*marker)->at("head").get<std::int64_t>();
        }
        return boundary;
    }

    /**
     * Places the queued items a boundary selects: every write, the first or all steers, and at `final` the first or
     * all follow-ups. A selected reset turns a `postTools` boundary into `final`. Writes are placed first and user
     * items after them, each in id order, so user items queued before a reset run in the new context. A write whose
     * head targets an entry before the active range, including a range started earlier in this commit, is stale.
     * Selected and stale items are removed positionally.
     */
    Result<BoundaryResult> apply(Transaction& tx, Boundary& boundary, const std::string& at, std::int64_t now) const {
        Json& items = (*boundary.inbox)["items"];
        bool reset = false;
        std::vector<std::size_t> writes;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].at("mode") == "write") {
                writes.push_back(i);
                reset = reset || items[i].at("entry").value("head", Json(nullptr)) == "self";
            }
        }
        const bool final = at == "final" || reset;
        std::vector<std::size_t> users = pick(items, "steer", boundary.steeringMode);
        if (final) {
            const std::vector<std::size_t> followUps = pick(items, "followUp", boundary.followUpMode);
            users.insert(users.end(), followUps.begin(), followUps.end());
        }
        std::sort(users.begin(), users.end());

        for (const std::size_t index : writes) {
            if (auto placed = placeWrite(tx, boundary, items[index]); !placed) {
                return std::unexpected(placed.error());
            }
        }
        BoundaryResult result;
        result.reset = reset;
        for (const std::size_t index : users) {
            auto placed = placeUser(tx, boundary.conversationId, items[index], now);
            if (!placed) {
                return std::unexpected(placed.error());
            }
            result.users.push_back(*placed);
        }
        std::vector<std::size_t> removed = writes;
        removed.insert(removed.end(), users.begin(), users.end());
        std::sort(removed.begin(), removed.end(), std::greater<>());
        for (const std::size_t index : removed) {
            items.erase(index);
        }
        return result;
    }

    /** Whether a head write targets an entry before the active range, so placing it would bring back cut history. */
    bool isStale(const Boundary& boundary, const Json& entry) const {
        return entry.contains("head") && entry.at("head").is_number_integer() && boundary.head &&
               entry.at("head").get<std::int64_t>() < *boundary.head;
    }

    /** Removes a withdrawn submission's item; the caller settles the submission. */
    Result<void> removeItem(Transaction& tx, std::int64_t conversationId, std::int64_t id) const {
        auto inbox = tx.doc(m_documents.inbox(), owner(conversationId));
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        Json& items = (**inbox)["items"];
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].at("id").get<std::int64_t>() == id) {
                items.erase(i);
                break;
            }
        }
        return {};
    }

    /**
     * Withdraws every queued input of a conversation, as conversation abort and abort cascades do: each settles
     * `unanswered` with `aborted` and leaves the inbox; queued writes stay for later placement.
     */
    Result<void> withdrawQueuedInputs(Transaction& tx, std::int64_t conversationId) const {
        auto inbox = tx.doc(m_documents.inbox(), owner(conversationId));
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        Json& items = (**inbox)["items"];
        for (std::size_t i = items.size(); i > 0; --i) {
            const std::size_t index = i - 1;
            if (items[index].at("mode") == "write") {
                continue;
            }
            if (auto settled = tx.settleSubmission(items[index].at("id").get<std::int64_t>(),
                                                   Json::object({{"status", "unanswered"}, {"reason", "aborted"}}));
                !settled) {
                return settled;
            }
            items.erase(index);
        }
        return {};
    }

private:
    DocAddressArgs owner(std::int64_t conversationId) const {
        DocAddressArgs args;
        args.owner = conversationId;
        return args;
    }

    std::vector<std::size_t> pick(const Json& items, const std::string& mode, const std::string& queueMode) const {
        std::vector<std::size_t> indexes;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].at("mode") == mode) {
                indexes.push_back(i);
            }
        }
        if (queueMode != "all" && indexes.size() > 1) {
            indexes.resize(1);
        }
        return indexes;
    }

    Result<void> placeWrite(Transaction& tx, Boundary& boundary, const Json& item) const {
        const Json draft = item.at("entry");
        const std::int64_t id = item.at("id").get<std::int64_t>();
        if (isStale(boundary, draft)) {
            return tx.settleSubmission(id, Json::object({{"status", "unanswered"}, {"reason", "stale"}}));
        }
        auto entry = tx.appendEntry(boundary.conversationId, draft);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (draft.contains("head")) {
            boundary.head = draft.at("head") == "self" ? entry->at("id").get<std::int64_t>() : draft.at("head").get<std::int64_t>();
        }
        return tx.placeSubmission(id, entry->at("id").get<std::int64_t>());
    }

    Result<std::int64_t> placeUser(Transaction& tx, std::int64_t conversationId, const Json& item, std::int64_t now) const {
        const Json message = Json::object({{"role", "user"}, {"content", item.at("content")}, {"timestamp", now}});
        auto entry = tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.user()}, {"model", Json::array({message})}}));
        if (!entry) {
            return std::unexpected(entry.error());
        }
        const std::int64_t id = item.at("id").get<std::int64_t>();
        if (auto placed = tx.placeSubmission(id, entry->at("id").get<std::int64_t>()); !placed) {
            return std::unexpected(placed.error());
        }
        return id;
    }

    BuiltinDocuments m_documents;
    EntryKinds m_kinds;
};
