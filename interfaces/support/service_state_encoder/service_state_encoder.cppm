export module pi.support.service_state_encoder;

import std;
export import pi.support.delta_encoder;
export import pi.types.json;
export import pi.types.result;

/**
 * The stateful operation encoders of every replicated state in ONE service subscription: each
 * (instance, member) state has its own path dictionary, created when a snapshot or spawn names it,
 * reset by `reset`/`replaced`/`unavailable` and dropped by `closed`. Works on the JSON forms of
 * ServiceSubscriptionSnapshot and ServiceProviderUpdate. Port of createServiceStateEncoder in
 * packages/chord/src/services/state-codec.ts. Errors have the code "service_state".
 */
export class ServiceStateEncoder {
public:
    Result<Json> encodeSnapshot(const Json& snapshot);
    Result<Json> encodeUpdate(const Json& update);

private:
    Result<Json> encodeInstance(const Json& instance);
    Result<DeltaEncoder*> add(const Json& instance, const std::string& member);
    Result<DeltaEncoder*> get(const Json& instance, const std::string& member);
    void removeInstance(const Json& instance);
    std::string stateKey(const Json& instance, const std::string& member) const;
    std::string describe(const Json& instance, const std::string& member) const;

    /** State key to (instance address or null, encoder). */
    std::map<std::string, std::pair<Json, DeltaEncoder>> m_encoders;
};

std::string ServiceStateEncoder::stateKey(const Json& instance, const std::string& member) const {
    const Json key = instance.is_object() ? instance["key"] : Json();
    const Json generation = instance.is_object() ? instance["generation"] : Json();
    return Json::array({key, generation, member}).dump();
}

std::string ServiceStateEncoder::describe(const Json& instance, const std::string& member) const {
    if (!instance.is_object()) {
        return member;
    }
    return instance["key"].get<std::string>() + "@" + instance["generation"].dump() + "." + member;
}

Result<DeltaEncoder*> ServiceStateEncoder::add(const Json& instance, const std::string& member) {
    const std::string key = stateKey(instance, member);
    if (m_encoders.contains(key)) {
        return std::unexpected(Error{"service_state", "Duplicate service state " + describe(instance, member)});
    }
    auto& entry = m_encoders[key];
    entry.first = instance;
    return &entry.second;
}

Result<DeltaEncoder*> ServiceStateEncoder::get(const Json& instance, const std::string& member) {
    const auto found = m_encoders.find(stateKey(instance, member));
    if (found == m_encoders.end()) {
        return std::unexpected(Error{"service_state", "Unknown service state " + describe(instance, member)});
    }
    return &found->second.second;
}

void ServiceStateEncoder::removeInstance(const Json& instance) {
    for (auto it = m_encoders.begin(); it != m_encoders.end();) {
        const Json& address = it->second.first;
        const bool same = address.is_object() && address["key"] == instance["key"] &&
                          address["generation"] == instance["generation"];
        it = same ? m_encoders.erase(it) : std::next(it);
    }
}

Result<Json> ServiceStateEncoder::encodeInstance(const Json& instance) {
    Json out = instance;
    const Json address = instance.contains("instance") ? instance["instance"] : Json();
    for (Json& member : out["members"]) {
        if (member["kind"] != "state") {
            continue;
        }
        auto encoder = add(address, member["name"].get<std::string>());
        if (!encoder) {
            return std::unexpected(encoder.error());
        }
        member["ops"] = (*encoder)->encode(member["ops"]);
    }
    return out;
}

Result<Json> ServiceStateEncoder::encodeSnapshot(const Json& snapshot) {
    m_encoders.clear();
    Json out = snapshot;
    out["instances"] = Json::array();
    for (const Json& instance : snapshot["instances"]) {
        auto encoded = encodeInstance(instance);
        if (!encoded) {
            return encoded;
        }
        out["instances"].push_back(std::move(*encoded));
    }
    return out;
}

Result<Json> ServiceStateEncoder::encodeUpdate(const Json& update) {
    const std::string type = update["type"].get<std::string>();
    if (type == "reset") {
        Json out = update;
        auto snapshot = encodeSnapshot(update["snapshot"]);
        if (!snapshot) {
            return snapshot;
        }
        out["snapshot"] = std::move(*snapshot);
        return out;
    }
    if (type == "state") {
        const Json address = update.contains("instance") ? update["instance"] : Json();
        auto encoder = get(address, update["member"].get<std::string>());
        if (!encoder) {
            return std::unexpected(encoder.error());
        }
        Json out = update;
        out["ops"] = (*encoder)->encode(update["ops"]);
        return out;
    }
    if (type == "replaced") {
        m_encoders.clear();
        auto snapshot = encodeInstance(update["snapshot"]);
        if (!snapshot) {
            return snapshot;
        }
        Json out = update;
        out["snapshot"] = std::move(*snapshot);
        return out;
    }
    if (type == "spawned") {
        auto instance = encodeInstance(update["instance"]);
        if (!instance) {
            return instance;
        }
        Json out = update;
        out["instance"] = std::move(*instance);
        return out;
    }
    if (type == "unavailable") {
        m_encoders.clear();
        return update;
    }
    if (type == "closed") {
        removeInstance(update["instance"]);
        return update;
    }
    return std::unexpected(Error{"service_state", "Unknown service update type " + type});
}
