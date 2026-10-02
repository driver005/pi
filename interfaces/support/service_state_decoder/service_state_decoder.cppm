module;

#include <nlohmann/json.hpp>

export module pi.support.service_state_decoder;

import std;
export import pi.support.delta_decoder;
export import pi.types.json;
export import pi.types.result;

/** Inverse of ServiceStateEncoder: turns wire snapshots and updates back into decoded ops. */
export class ServiceStateDecoder {
public:
    Result<Json> decodeSnapshot(const Json& snapshot);
    Result<Json> decodeUpdate(const Json& update);

private:
    Result<Json> decodeInstance(const Json& instance);
    Result<DeltaDecoder*> add(const Json& instance, const std::string& member);
    Result<DeltaDecoder*> get(const Json& instance, const std::string& member);
    void removeInstance(const Json& instance);
    std::string stateKey(const Json& instance, const std::string& member) const;
    std::string describe(const Json& instance, const std::string& member) const;

    std::map<std::string, std::pair<Json, DeltaDecoder>> m_decoders;
};

std::string ServiceStateDecoder::stateKey(const Json& instance, const std::string& member) const {
    const Json key = instance.is_object() ? instance["key"] : Json();
    const Json generation = instance.is_object() ? instance["generation"] : Json();
    return Json::array({key, generation, member}).dump();
}

std::string ServiceStateDecoder::describe(const Json& instance, const std::string& member) const {
    if (!instance.is_object()) {
        return member;
    }
    return instance["key"].get<std::string>() + "@" + instance["generation"].dump() + "." + member;
}

Result<DeltaDecoder*> ServiceStateDecoder::add(const Json& instance, const std::string& member) {
    const std::string key = stateKey(instance, member);
    if (m_decoders.contains(key)) {
        return std::unexpected(Error{"service_state", "Duplicate service state " + describe(instance, member)});
    }
    auto& entry = m_decoders[key];
    entry.first = instance;
    return &entry.second;
}

Result<DeltaDecoder*> ServiceStateDecoder::get(const Json& instance, const std::string& member) {
    const auto found = m_decoders.find(stateKey(instance, member));
    if (found == m_decoders.end()) {
        return std::unexpected(Error{"service_state", "Unknown service state " + describe(instance, member)});
    }
    return &found->second.second;
}

void ServiceStateDecoder::removeInstance(const Json& instance) {
    for (auto it = m_decoders.begin(); it != m_decoders.end();) {
        const Json& address = it->second.first;
        const bool same = address.is_object() && address["key"] == instance["key"] &&
                          address["generation"] == instance["generation"];
        it = same ? m_decoders.erase(it) : std::next(it);
    }
}

Result<Json> ServiceStateDecoder::decodeInstance(const Json& instance) {
    Json out = instance;
    const Json address = instance.contains("instance") ? instance["instance"] : Json();
    for (Json& member : out["members"]) {
        if (member["kind"] != "state") {
            continue;
        }
        auto decoder = add(address, member["name"].get<std::string>());
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        auto ops = (*decoder)->decode(member["ops"]);
        if (!ops) {
            return std::unexpected(ops.error());
        }
        member["ops"] = std::move(*ops);
    }
    return out;
}

Result<Json> ServiceStateDecoder::decodeSnapshot(const Json& snapshot) {
    m_decoders.clear();
    Json out = snapshot;
    out["instances"] = Json::array();
    for (const Json& instance : snapshot["instances"]) {
        auto decoded = decodeInstance(instance);
        if (!decoded) {
            return decoded;
        }
        out["instances"].push_back(std::move(*decoded));
    }
    return out;
}

Result<Json> ServiceStateDecoder::decodeUpdate(const Json& update) {
    const std::string type = update["type"].get<std::string>();
    Json out = update;
    if (type == "reset") {
        auto snapshot = decodeSnapshot(update["snapshot"]);
        if (!snapshot) {
            return snapshot;
        }
        out["snapshot"] = std::move(*snapshot);
        return out;
    }
    if (type == "state") {
        const Json address = update.contains("instance") ? update["instance"] : Json();
        auto decoder = get(address, update["member"].get<std::string>());
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        auto ops = (*decoder)->decode(update["ops"]);
        if (!ops) {
            return std::unexpected(ops.error());
        }
        out["ops"] = std::move(*ops);
        return out;
    }
    if (type == "replaced") {
        m_decoders.clear();
        auto snapshot = decodeInstance(update["snapshot"]);
        if (!snapshot) {
            return snapshot;
        }
        out["snapshot"] = std::move(*snapshot);
        return out;
    }
    if (type == "spawned") {
        auto instance = decodeInstance(update["instance"]);
        if (!instance) {
            return instance;
        }
        out["instance"] = std::move(*instance);
        return out;
    }
    if (type == "unavailable") {
        m_decoders.clear();
        return update;
    }
    if (type == "closed") {
        removeInstance(update["instance"]);
        return update;
    }
    return std::unexpected(Error{"service_state", "Unknown service update type " + type});
}
