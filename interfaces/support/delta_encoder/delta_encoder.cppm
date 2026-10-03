export module pi.support.delta_encoder;

import std;
export import pi.types.json;

/**
 * Turns decoded delta ops into wire ops for ONE independent state stream: a path is sent inline on
 * its first use, defined (`["#", id, path]`) and referenced by id from its second use on, and an op
 * on the same path as the previous op of its batch drops the path (arity tells the decoder). A base
 * batch (`["r", value]`) resets the dictionary, so every batch after it is self-contained. Port of
 * encoder() in packages/chord/src/delta/index.ts.
 */
export class DeltaEncoder {
public:
    /** `ops` is a Json array of decoded ops; returns the wire batch. */
    Json encode(const Json& ops);

private:
    Json shorten(const Json& op) const;
    Json withReference(const Json& op, const Json& reference) const;

    std::set<std::string> m_seen;
    std::map<std::string, std::int64_t> m_ids;
    std::int64_t m_nextId = 0;
};

Json DeltaEncoder::shorten(const Json& op) const {
    const std::string verb = op[0].get<std::string>();
    if (verb == "d") {
        return Json::array({"d"});
    }
    if (verb == "p") {
        return Json::array({"p", op[2], op[3], op[4]});
    }
    return Json::array({verb, op[2]});
}

Json DeltaEncoder::withReference(const Json& op, const Json& reference) const {
    const std::string verb = op[0].get<std::string>();
    if (verb == "d") {
        return Json::array({"d", reference});
    }
    if (verb == "p") {
        return Json::array({"p", reference, op[2], op[3], op[4]});
    }
    return Json::array({verb, reference, op[2]});
}

Json DeltaEncoder::encode(const Json& ops) {
    // Path omission is scoped to a batch; ids are the only cross-batch state.
    std::optional<std::string> previous;
    Json out = Json::array();
    for (const Json& op : ops) {
        if (op[0] == "r") {
            out.push_back(op);
            // A base batch is a recovery point: later batches must not refer to earlier definitions.
            m_seen.clear();
            m_ids.clear();
            m_nextId = 0;
            previous.reset();
            continue;
        }
        const Json& path = op[1];
        const std::string key = path.dump();
        if (previous && *previous == key) {
            out.push_back(shorten(op));
            continue;
        }
        Json reference = path;
        const auto existing = m_ids.find(key);
        if (existing != m_ids.end()) {
            reference = existing->second;
        } else if (m_seen.contains(key)) {
            const std::int64_t id = m_nextId++;
            m_ids[key] = id;
            out.push_back(Json::array({"#", id, path}));
            reference = id;
        } else {
            m_seen.insert(key);
        }
        out.push_back(withReference(op, reference));
        previous = key;
    }
    return out;
}
