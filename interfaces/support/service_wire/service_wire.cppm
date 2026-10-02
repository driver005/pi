module;

#include <nlohmann/json.hpp>

#include <cmath>

export module pi.support.service_wire;

import std;
export import pi.support.delta_validator;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_control_call;

/**
 * The service call and update grammar of the protocol's opaque payloads: `{serviceId, instance?,
 * member, args}` calls, the `$chord.service` control calls, catalogue entries, subscription
 * snapshots and provider updates (decoded form carries full ops, wire form path ids and short
 * forms). Validation failures have the code "invalid_request". Port of
 * packages/chord/src/services/wire.ts.
 */
export class ServiceWire {
public:
    static constexpr std::string_view kControlId = "$chord.service";

    Json catalogueCall() const;
    Json subscribeCall(const std::string& subscriptionId, const std::string& serviceId, const std::string& mode) const;
    Json unsubscribeCall(const std::string& subscriptionId) const;
    /** The control call a service call encodes, or nullopt for an ordinary call. */
    std::optional<ServiceControlCall> decodeControl(const Json& call) const;

    Result<void> validateCall(const Json& value) const;
    Result<void> validateCatalogue(const Json& value) const;
    Result<void> validateSubscriptionSnapshot(const Json& value, bool wire) const;
    Result<void> validateUpdate(const Json& value, bool wire) const;

private:
    Result<void> validateInstanceSnapshot(const Json& value, bool wire, const std::string& what) const;
    Result<void> validateAddress(const Json& value) const;
    Result<void> validateOps(const Json& ops, bool wire) const;
    bool keys(const Json& object, std::initializer_list<std::string_view> required,
              std::initializer_list<std::string_view> optional = {}) const;
    bool id(const Json& value) const;
    bool mode(const Json& value) const;
    bool integer(const Json& value, double minimum) const;
    Error invalid(const std::string& what) const;

    DeltaValidator m_ops;
};

Error ServiceWire::invalid(const std::string& what) const {
    return Error{"invalid_request", "Invalid " + what};
}

bool ServiceWire::id(const Json& value) const {
    return value.is_string() && !value.get_ref<const std::string&>().empty();
}

bool ServiceWire::mode(const Json& value) const {
    return value == "singleton" || value == "keyed";
}

bool ServiceWire::integer(const Json& value, double minimum) const {
    if (!value.is_number()) {
        return false;
    }
    const double number = value.get<double>();
    return std::isfinite(number) && std::floor(number) == number && number >= minimum;
}

bool ServiceWire::keys(const Json& object, std::initializer_list<std::string_view> required,
                       std::initializer_list<std::string_view> optional) const {
    if (!object.is_object()) {
        return false;
    }
    for (const std::string_view key : required) {
        if (!object.contains(std::string(key))) {
            return false;
        }
    }
    for (const auto& entry : object.items()) {
        const std::string_view key = entry.key();
        const bool known = std::find(required.begin(), required.end(), key) != required.end() ||
                           std::find(optional.begin(), optional.end(), key) != optional.end();
        if (!known) {
            return false;
        }
    }
    return true;
}

Json ServiceWire::catalogueCall() const {
    return Json{{"serviceId", std::string(kControlId)}, {"member", "catalogue"}, {"args", Json::array()}};
}

Json ServiceWire::subscribeCall(const std::string& subscriptionId, const std::string& serviceId,
                                const std::string& mode) const {
    return Json{{"serviceId", std::string(kControlId)},
                {"member", "subscribe"},
                {"args", Json::array({subscriptionId, serviceId, mode})}};
}

Json ServiceWire::unsubscribeCall(const std::string& subscriptionId) const {
    return Json{{"serviceId", std::string(kControlId)}, {"member", "unsubscribe"}, {"args", Json::array({subscriptionId})}};
}

std::optional<ServiceControlCall> ServiceWire::decodeControl(const Json& call) const {
    if (!call.is_object() || call.value("serviceId", Json()) != std::string(kControlId) || call.contains("instance")) {
        return std::nullopt;
    }
    const Json member = call.value("member", Json());
    const Json args = call.value("args", Json::array());
    if (!args.is_array()) {
        return std::nullopt;
    }
    ServiceControlCall control;
    if (member == "catalogue" && args.empty()) {
        control.type = "catalogue";
        return control;
    }
    if (member == "subscribe" && args.size() == 3 && id(args[0]) && id(args[1]) && mode(args[2])) {
        control.type = "subscribe";
        control.subscriptionId = args[0].get<std::string>();
        control.serviceId = args[1].get<std::string>();
        control.mode = args[2].get<std::string>();
        return control;
    }
    if (member == "unsubscribe" && args.size() == 1 && id(args[0])) {
        control.type = "unsubscribe";
        control.subscriptionId = args[0].get<std::string>();
        return control;
    }
    return std::nullopt;
}

Result<void> ServiceWire::validateAddress(const Json& value) const {
    if (!keys(value, {"key", "generation"}) || !id(value["key"]) || !integer(value["generation"], 1)) {
        return std::unexpected(invalid("service instance address"));
    }
    return {};
}

Result<void> ServiceWire::validateCall(const Json& value) const {
    if (!keys(value, {"serviceId", "member", "args"}, {"instance"}) || !id(value["serviceId"]) || !id(value["member"]) ||
        !value["args"].is_array()) {
        return std::unexpected(invalid("service call"));
    }
    if (value.contains("instance")) {
        return validateAddress(value["instance"]);
    }
    return {};
}

Result<void> ServiceWire::validateCatalogue(const Json& value) const {
    if (!value.is_array()) {
        return std::unexpected(invalid("service catalogue"));
    }
    std::set<std::string> ids;
    for (const Json& entry : value) {
        if (!keys(entry, {"serviceId", "mode"}) || !id(entry["serviceId"]) || !mode(entry["mode"]) ||
            !ids.insert(entry["serviceId"].get<std::string>()).second) {
            return std::unexpected(invalid("service catalogue"));
        }
    }
    return {};
}

Result<void> ServiceWire::validateOps(const Json& ops, bool wire) const {
    if (!ops.is_array()) {
        return std::unexpected(invalid("service state update"));
    }
    for (const Json& op : ops) {
        auto valid = wire ? m_ops.validateWireOp(op) : m_ops.validateOp(op);
        if (!valid) {
            return std::unexpected(Error{"invalid_request", valid.error().message});
        }
    }
    return {};
}

Result<void> ServiceWire::validateInstanceSnapshot(const Json& value, bool wire, const std::string& what) const {
    if (!keys(value, {"members"}, {"instance"}) || !value["members"].is_array()) {
        return std::unexpected(invalid(what));
    }
    if (value.contains("instance")) {
        if (auto address = validateAddress(value["instance"]); !address) {
            return address;
        }
    }
    for (const Json& member : value["members"]) {
        if (!member.is_object()) {
            return std::unexpected(invalid("service member snapshot"));
        }
        if (member.value("kind", Json()) == "method") {
            if (!keys(member, {"name", "kind"}) || !id(member["name"])) {
                return std::unexpected(invalid("service method snapshot"));
            }
        } else if (member.value("kind", Json()) == "state") {
            if (!keys(member, {"name", "kind", "sequence", "ops"}) || !id(member["name"]) ||
                !integer(member["sequence"], 0) || !member["ops"].is_array()) {
                return std::unexpected(invalid("service state snapshot"));
            }
            if (auto ops = validateOps(member["ops"], wire); !ops) {
                return ops;
            }
        } else {
            return std::unexpected(invalid("service member snapshot"));
        }
    }
    return {};
}

Result<void> ServiceWire::validateSubscriptionSnapshot(const Json& value, bool wire) const {
    if (!keys(value, {"serviceId", "mode", "instances"}) || !id(value["serviceId"]) || !mode(value["mode"]) ||
        !value["instances"].is_array()) {
        return std::unexpected(invalid("service subscription snapshot"));
    }
    for (const Json& instance : value["instances"]) {
        if (auto valid = validateInstanceSnapshot(instance, wire, "service instance snapshot"); !valid) {
            return valid;
        }
    }
    return {};
}

Result<void> ServiceWire::validateUpdate(const Json& value, bool wire) const {
    if (!value.is_object() || !value.contains("type") || !value["type"].is_string()) {
        return std::unexpected(invalid("service provider update"));
    }
    const std::string type = value["type"].get<std::string>();
    if (type == "state") {
        if (!keys(value, {"type", "member", "sequence", "ops"}, {"instance"}) || !id(value["member"]) ||
            !integer(value["sequence"], 1) || !value["ops"].is_array()) {
            return std::unexpected(invalid("service state update"));
        }
        if (value.contains("instance")) {
            if (auto address = validateAddress(value["instance"]); !address) {
                return address;
            }
        }
        return validateOps(value["ops"], wire);
    }
    if (type == "reset") {
        if (!keys(value, {"type", "snapshot"})) {
            return std::unexpected(invalid("reset update"));
        }
        if (auto snapshot = validateSubscriptionSnapshot(value["snapshot"], wire); !snapshot) {
            return snapshot;
        }
        for (const Json& instance : value["snapshot"]["instances"]) {
            for (const Json& member : instance["members"]) {
                if (member["kind"] == "state" && (member["ops"].size() != 1 || member["ops"][0][0] != "r")) {
                    return std::unexpected(Error{"invalid_request", "Service reset must contain full root replacements"});
                }
            }
        }
        return {};
    }
    if (type == "unavailable") {
        return keys(value, {"type"}) ? Result<void>() : std::unexpected(invalid("unavailable update"));
    }
    if (type == "replaced") {
        if (!keys(value, {"type", "snapshot"})) {
            return std::unexpected(invalid("replacement update"));
        }
        return validateInstanceSnapshot(value["snapshot"], wire, "service instance snapshot");
    }
    if (type == "spawned") {
        if (!keys(value, {"type", "instance"})) {
            return std::unexpected(invalid("spawn update"));
        }
        return validateInstanceSnapshot(value["instance"], wire, "service instance snapshot");
    }
    if (type == "closed") {
        if (!keys(value, {"type", "instance"})) {
            return std::unexpected(invalid("close update"));
        }
        return validateAddress(value["instance"]);
    }
    return std::unexpected(invalid("service provider update"));
}
