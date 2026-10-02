module;

#include <nlohmann/json.hpp>

#include <cmath>

export module pi.support.protocol_validator;

import std;
export import pi.support.json_value_checker;
export import pi.types.json;
export import pi.types.protocol_side;
export import pi.types.result;

/**
 * Structural validation of protocol v8 messages: strict objects (no unknown fields), non-empty ids,
 * canonical UUIDv4 server ids, strict-JSON opaque payloads. Port of the TypeBox schemas in
 * packages/protocol/src/protocol.ts.
 */
export class ProtocolValidator {
public:
    static constexpr int kProtocolVersion = 8;

    /** The Error message is "Invalid <client|server> protocol message". */
    Result<void> validate(ProtocolSide side, const Json& message) const;
    bool supportedVersion(const Json& version) const;
    bool serverId(const Json& value) const;

private:
    bool clientMessage(const Json& message) const;
    bool serverMessage(const Json& message) const;
    bool response(const Json& message) const;
    bool exactKeys(const Json& object, std::initializer_list<std::string_view> required,
                   std::initializer_list<std::string_view> optional = {}) const;
    bool id(const Json& value) const;
    bool integer(const Json& value) const;
    bool text(const Json& value, std::string_view key, std::string_view expected) const;
    bool target(const Json& value) const;
    bool sessionTarget(const Json& value) const;
    bool protocolError(const Json& value) const;
    bool hexRun(const std::string& text, std::size_t from, std::size_t count) const;

    JsonValueChecker m_json;
};

bool ProtocolValidator::exactKeys(const Json& object, std::initializer_list<std::string_view> required,
                                  std::initializer_list<std::string_view> optional) const {
    if (!object.is_object()) {
        return false;
    }
    const auto contains = [](std::initializer_list<std::string_view> keys, const std::string& key) {
        return std::find(keys.begin(), keys.end(), std::string_view(key)) != keys.end();
    };
    for (const std::string_view key : required) {
        if (!object.contains(std::string(key))) {
            return false;
        }
    }
    for (const auto& entry : object.items()) {
        if (!contains(required, entry.key()) && !contains(optional, entry.key())) {
            return false;
        }
    }
    return true;
}

bool ProtocolValidator::id(const Json& value) const {
    return value.is_string() && !value.get_ref<const std::string&>().empty();
}

bool ProtocolValidator::integer(const Json& value) const {
    if (value.is_number_integer()) {
        return true;
    }
    return value.is_number_float() && std::isfinite(value.get<double>()) && std::floor(value.get<double>()) == value.get<double>();
}

bool ProtocolValidator::text(const Json& value, std::string_view key, std::string_view expected) const {
    const std::string name(key);
    return value.contains(name) && value[name].is_string() && value[name].get_ref<const std::string&>() == expected;
}

bool ProtocolValidator::hexRun(const std::string& text, std::size_t from, std::size_t count) const {
    for (std::size_t i = from; i < from + count; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool ProtocolValidator::serverId(const Json& value) const {
    if (!value.is_string()) {
        return false;
    }
    const std::string& text = value.get_ref<const std::string&>();
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return false;
    }
    return hexRun(text, 0, 8) && hexRun(text, 9, 4) && text[14] == '4' && hexRun(text, 15, 3) &&
           (text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b') && hexRun(text, 20, 3) &&
           hexRun(text, 24, 12);
}

bool ProtocolValidator::supportedVersion(const Json& version) const {
    return integer(version) && version.get<double>() == kProtocolVersion;
}

bool ProtocolValidator::sessionTarget(const Json& value) const {
    return exactKeys(value, {"serverId", "sessionId", "attachmentId"}) && serverId(value["serverId"]) &&
           id(value["sessionId"]) && id(value["attachmentId"]);
}

bool ProtocolValidator::target(const Json& value) const {
    if (exactKeys(value, {"serverId"})) {
        return serverId(value["serverId"]);
    }
    return sessionTarget(value);
}

bool ProtocolValidator::protocolError(const Json& value) const {
    return exactKeys(value, {"code", "message"}) && id(value["code"]) && value["message"].is_string();
}

bool ProtocolValidator::clientMessage(const Json& message) const {
    if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) {
        return false;
    }
    const std::string& type = message["type"].get_ref<const std::string&>();
    if (type == "hello") {
        return exactKeys(message, {"type", "version"}) && integer(message["version"]) && message["version"].get<double>() >= 0;
    }
    if (type == "request") {
        return exactKeys(message, {"type", "id", "target", "call"}) && id(message["id"]) && target(message["target"]) &&
               m_json.valid(message["call"]);
    }
    if (type == "cancel") {
        return exactKeys(message, {"type", "id", "target"}) && id(message["id"]) && target(message["target"]);
    }
    return false;
}

bool ProtocolValidator::response(const Json& message) const {
    if (!message.contains("ok") || !message["ok"].is_boolean() || !message.contains("id") || !id(message["id"])) {
        return false;
    }
    if (message["ok"].get<bool>()) {
        return exactKeys(message, {"type", "id", "ok"}, {"result"}) && (!message.contains("result") || m_json.valid(message["result"]));
    }
    return exactKeys(message, {"type", "id", "ok", "error"}) && protocolError(message["error"]);
}

bool ProtocolValidator::serverMessage(const Json& message) const {
    if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) {
        return false;
    }
    const std::string& type = message["type"].get_ref<const std::string&>();
    if (type == "hello") {
        return exactKeys(message, {"type", "version", "serverId"}) && supportedVersion(message["version"]) &&
               serverId(message["serverId"]);
    }
    if (type == "hello_error") {
        return exactKeys(message, {"type", "error"}) && protocolError(message["error"]);
    }
    if (type == "response") {
        return response(message);
    }
    if (type == "service_update") {
        return exactKeys(message, {"type", "subscriptionId", "update"}) && id(message["subscriptionId"]) &&
               m_json.valid(message["update"]);
    }
    if (type == "attachment") {
        return exactKeys(message, {"type", "attachment"}) &&
               (message["attachment"].is_null() || sessionTarget(message["attachment"]));
    }
    return false;
}

Result<void> ProtocolValidator::validate(ProtocolSide side, const Json& message) const {
    const bool ok = side == ProtocolSide::Client ? clientMessage(message) : serverMessage(message);
    if (ok) {
        return {};
    }
    return std::unexpected(Error{"protocol_validation", std::string("Invalid ") +
                                                            (side == ProtocolSide::Client ? "client" : "server") +
                                                            " protocol message"});
}
