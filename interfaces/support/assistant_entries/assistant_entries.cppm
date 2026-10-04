module;

#include <cstdint>

export module pi.support.assistant_entries;

import std;
export import pi.support.entry_kinds;
export import pi.support.transaction;
export import pi.support.usage_ledger;
export import pi.types.json;
export import pi.types.result;

/**
 * Appends provider results as `pi.assistant` entries. Every built-in writer of assistant entries goes through here, so
 * the usage ledger stays complete: a result's usage is added to `pi.usage` in the same commit.
 */
export class AssistantEntries {
public:
    /** Appends `message` (a JSON assistant message) and records its usage under `provider/model`; returns the entry. */
    Result<Json> append(Transaction& tx, std::int64_t conversationId, const Json& message) const {
        if (message.contains("usage")) {
            const std::string key = message.value("provider", std::string()) + "/" + message.value("model", std::string());
            if (auto recorded = m_ledger.record(tx, conversationId, "models", key, message.at("usage")); !recorded) {
                return std::unexpected(recorded.error());
            }
        }
        return tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.assistant()}, {"model", Json::array({message})}}));
    }

    /**
     * Appends a committed partial left by an interrupted, aborted, faulted or orphaned attempt as an aborted assistant
     * entry; the caller replaces or removes `generation`.
     */
    Result<void> convertPartial(Transaction& tx, const Json& live, std::int64_t conversationId) const {
        if (!live.contains("generation") || !live.at("generation").contains("message")) {
            return {};
        }
        Json message = live.at("generation").at("message");
        message["stopReason"] = "aborted";
        auto appended = append(tx, conversationId, message);
        return appended ? Result<void>() : std::unexpected(appended.error());
    }

private:
    EntryKinds m_kinds;
    UsageLedger m_ledger;
};
