export module pi.support.telemetry_schema_validator;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * Checks span data against a telemetry schema in the serializable form of packages/telemetry (`defineTelemetrySchema`):
 * `{version, spans: {<name>: {description, parents: {kind: "any"|"root_or_external"|"spans", spans?}, startAttributes,
 * endAttributes, events?}}}`, where an attribute is `{type: "string"|"number"|"boolean"|"string[]"|"number[]"|"boolean[]",
 * required?, values?, elementValues?}`. TypeScript applies the schema as compile-time types only; this class does the same
 * checking at run time: unknown attributes, wrong types, values outside `values`/`elementValues` and missing required
 * attributes are errors.
 */
export class TelemetrySchemaValidator {
public:
    explicit TelemetrySchemaValidator(Json schema)
        : m_schema(std::move(schema)) {}

    /** The schema itself: an object with a version and spans that each have the sections above. */
    Result<void> validateSchema() const {
        if (!m_schema.is_object() || !m_schema.contains("version") || !m_schema["version"].is_number_integer() || !m_schema.contains("spans") || !m_schema["spans"].is_object()) {
            return fail("a telemetry schema is an object with an integer version and a spans object");
        }
        for (const auto& entry : m_schema["spans"].items()) {
            const Json& span = entry.value();
            if (!span.is_object() || !span.contains("description") || !span["description"].is_string() || !span.contains("parents") || !span["parents"].is_object() || !span.contains("startAttributes") || !span["startAttributes"].is_object() || !span.contains("endAttributes") || !span["endAttributes"].is_object()) {
                return fail("span \"" + entry.key() + "\" needs description, parents, startAttributes and endAttributes");
            }
            const std::string kind = span["parents"].value("kind", "");
            if (kind != "any" && kind != "root_or_external" && kind != "spans") {
                return fail("span \"" + entry.key() + "\" has an unknown parents kind");
            }
        }
        return {};
    }

    /** The attributes a span starts with: required ones present, every one declared and well typed. */
    Result<void> validateStart(const std::string& span, const Json& attributes) const {
        const Json* definition = section(span, "startAttributes");
        return definition == nullptr ? unknownSpan(span) : check(span + " start", *definition, attributes, true);
    }

    /** The attributes a span sets before it ends: all optional. */
    Result<void> validateEnd(const std::string& span, const Json& attributes) const {
        const Json* definition = section(span, "endAttributes");
        return definition == nullptr ? unknownSpan(span) : check(span + " end", *definition, attributes, false);
    }

    Result<void> validateEvent(const std::string& span, const std::string& event, const Json& attributes) const {
        const Json* events = section(span, "events");
        if (events == nullptr || !events->contains(event) || !(*events)[event].is_object()) {
            return fail("span \"" + span + "\" has no event \"" + event + "\"");
        }
        const Json& definition = (*events)[event];
        return check(span + " event " + event, definition.contains("attributes") ? definition["attributes"] : Json::object(), attributes, true);
    }

    /** Whether `span` may run under `parent` (nullopt: no parent). */
    Result<void> validateParent(const std::string& span, const std::optional<std::string>& parent) const {
        if (!m_schema.is_object() || !m_schema.contains("spans") || !m_schema["spans"].contains(span)) {
            return unknownSpan(span);
        }
        const Json& parents = m_schema["spans"][span]["parents"];
        const std::string kind = parents.value("kind", "any");
        if (kind == "any") {
            return {};
        }
        if (kind == "root_or_external") {
            return parent ? fail("span \"" + span + "\" must be a root span") : Result<void>{};
        }
        if (parent && parents.contains("spans") && parents["spans"].is_array() && std::ranges::any_of(parents["spans"], [&](const Json& name) { return name.is_string() && name.get<std::string>() == *parent; })) {
            return {};
        }
        return fail("span \"" + span + "\" cannot run under " + (parent ? "\"" + *parent + "\"" : std::string("no parent")));
    }

private:
    const Json* section(const std::string& span, const std::string& name) const {
        if (!m_schema.is_object() || !m_schema.contains("spans") || !m_schema["spans"].contains(span) || !m_schema["spans"][span].contains(name)) {
            return nullptr;
        }
        return &m_schema["spans"][span][name];
    }

    Result<void> check(const std::string& where, const Json& definitions, const Json& attributes, bool requireRequired) const {
        if (!attributes.is_object()) {
            return fail(where + ": attributes must be an object");
        }
        for (const auto& entry : attributes.items()) {
            if (entry.value().is_null()) {
                continue;
            }
            if (!definitions.contains(entry.key())) {
                return fail(where + ": unknown attribute \"" + entry.key() + "\"");
            }
            if (auto typed = checkValue(where, entry.key(), definitions[entry.key()], entry.value()); !typed) {
                return typed;
            }
        }
        if (requireRequired) {
            for (const auto& entry : definitions.items()) {
                const bool required = entry.value().contains("required") && entry.value()["required"].is_boolean() && entry.value()["required"].get<bool>();
                if (required && (!attributes.contains(entry.key()) || attributes[entry.key()].is_null())) {
                    return fail(where + ": missing required attribute \"" + entry.key() + "\"");
                }
            }
        }
        return {};
    }

    Result<void> checkValue(const std::string& where, const std::string& name, const Json& definition, const Json& value) const {
        const std::string type = definition.value("type", "");
        const bool isArray = type.ends_with("[]");
        const std::string element = isArray ? type.substr(0, type.size() - 2) : type;
        if (isArray ? !value.is_array() : !matches(element, value)) {
            return fail(where + ": attribute \"" + name + "\" must be " + type);
        }
        const char* allowedKey = isArray ? "elementValues" : "values";
        if (isArray) {
            for (const Json& item : value) {
                if (!matches(element, item) || !allowed(definition, allowedKey, item)) {
                    return fail(where + ": attribute \"" + name + "\" has an invalid element");
                }
            }
            return {};
        }
        return allowed(definition, allowedKey, value) ? Result<void>{} : fail(where + ": attribute \"" + name + "\" is not one of its allowed values");
    }

    bool matches(const std::string& type, const Json& value) const {
        return (type == "string" && value.is_string()) || (type == "number" && value.is_number()) || (type == "boolean" && value.is_boolean());
    }

    bool allowed(const Json& definition, const char* key, const Json& value) const {
        if (!definition.contains(key) || !definition[key].is_array()) {
            return true;
        }
        return std::ranges::any_of(definition[key], [&value](const Json& candidate) { return candidate == value; });
    }

    Result<void> unknownSpan(const std::string& span) const {
        return fail("the schema has no span \"" + span + "\"");
    }

    Result<void> fail(const std::string& message) const {
        return std::unexpected(Error{"telemetry_schema", message});
    }

    Json m_schema;
};
