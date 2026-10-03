module;

#include <cstdint>

export module pi.support.usage_ledger;

import std;
export import pi.support.builtin_documents;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;

/**
 * The per-conversation spend ledger (`pi.usage`): usage is a JSON object of token counters and a `cost` object,
 * summed per `provider/modelId` for model calls and per tool name for tool results.
 */
export class UsageLedger {
public:
    /** Adds `usage` to one bucket ("models" or "tools") of the conversation's `pi.usage`, in the recording commit. */
    Result<void> record(Transaction& tx, std::int64_t conversationId, const std::string& bucket, const std::string& key,
                        const Json& usage) const {
        DocAddressArgs args;
        args.owner = conversationId;
        auto document = tx.doc(m_documents.usage(), args);
        if (!document) {
            return std::unexpected(document.error());
        }
        Json& totals = (**document)[bucket];
        if (!totals.contains(key)) {
            totals[key] = usage;
            return {};
        }
        add(totals[key], usage);
        return {};
    }

    /** Adds every counter of `usage` to `total`; optional counters are added once either side reports them. */
    void add(Json& total, const Json& usage) const {
        for (const char* counter : {"input", "output", "cacheRead", "cacheWrite", "totalTokens"}) {
            addNumber(total, counter, usage.contains(counter) ? usage.at(counter) : Json(0));
        }
        for (const char* optional : {"cacheWrite1h", "reasoning"}) {
            if (usage.contains(optional)) {
                addNumber(total, optional, usage.at(optional));
            }
        }
        Json& cost = total["cost"];
        for (const char* part : {"input", "output", "cacheRead", "cacheWrite", "total"}) {
            addNumber(cost, part, usage.contains("cost") && usage.at("cost").contains(part) ? usage.at("cost").at(part) : Json(0));
        }
    }

    /** Adds every bucket of `state` into `sum`. */
    void addState(Json& sum, const Json& state) const {
        for (const char* bucket : {"models", "tools"}) {
            if (!state.contains(bucket)) {
                continue;
            }
            for (const auto& entry : state.at(bucket).items()) {
                if (sum[bucket].contains(entry.key())) {
                    add(sum[bucket][entry.key()], entry.value());
                } else {
                    sum[bucket][entry.key()] = entry.value();
                }
            }
        }
    }

    /** An empty ledger. */
    Json empty() const {
        return m_documents.usage().initial(Json(nullptr));
    }

private:
    /** target[key] += value, staying integral while both sides are. */
    void addNumber(Json& target, const char* key, const Json& value) const {
        if (!target.contains(key)) {
            target[key] = value;
            return;
        }
        if (target.at(key).is_number_integer() && value.is_number_integer()) {
            target[key] = target.at(key).get<std::int64_t>() + value.get<std::int64_t>();
        } else {
            target[key] = target.at(key).get<double>() + value.get<double>();
        }
    }

    BuiltinDocuments m_documents;
};
