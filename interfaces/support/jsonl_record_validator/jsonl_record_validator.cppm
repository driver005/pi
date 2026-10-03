export module pi.support.jsonl_record_validator;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * Parses and validates the lines of the JSONL storage files: commit markers of `main.jsonl` and
 * records of the `doc-<id>.jsonl` / `task-<id>.jsonl` sidecars. Every failure has the code
 * "jsonl_corruption". Port of the parse/validate functions of packages/durable/src/storage/jsonl/storage.ts.
 */
export class JsonlRecordValidator {
public:
    static constexpr std::int64_t kFormatVersion = 1;

    Result<Json> parseMainMarker(const std::string& text, std::size_t line) const {
        const std::string description = "main.jsonl line " + std::to_string(line);
        auto value = parseJson(text, description);
        if (!value) {
            return value;
        }
        if (!value->is_object() || value->value("format", Json()) != kFormatVersion ||
            value->value("type", Json()) != "commit" || !safeInteger((*value)["seq"]) ||
            (*value)["seq"].get<std::int64_t>() < 1 || !(*value)["writes"].is_array()) {
            return std::unexpected(corrupt("Invalid commit marker in " + description));
        }
        for (Json& write : (*value)["writes"]) {
            if (auto valid = validateMainOperation(write, description); !valid) {
                return std::unexpected(valid.error());
            }
        }
        return value;
    }

    Result<Json> parseSidecarRecord(const std::string& text, const std::string& file, std::size_t line) const {
        const std::string description = file + " line " + std::to_string(line);
        auto value = parseJson(text, description);
        if (!value) {
            return value;
        }
        if (!value->is_object() || value->value("format", Json()) != kFormatVersion ||
            value->value("type", Json()) != "record" || !safeInteger((*value)["seq"]) ||
            (*value)["seq"].get<std::int64_t>() < 1 || !safeInteger((*value)["ordinal"]) ||
            (*value)["ordinal"].get<std::int64_t>() < 0 || !(*value)["payload"].is_object() ||
            !(*value)["payload"]["type"].is_string()) {
            return std::unexpected(corrupt("Invalid sidecar record in " + description));
        }
        Json& payload = (*value)["payload"];
        if (payload["type"] == "task") {
            Json& task = payload["value"];
            if (!task.is_object() || !safeInteger(task["id"]) || !task["state"].is_object() ||
                task["state"].value("status", Json()) == "terminal") {
                return std::unexpected(corrupt("Invalid live task record in " + description));
            }
        } else if (payload["type"] == "document") {
            if (!safeInteger(payload["id"])) {
                return std::unexpected(corrupt("Invalid document record in " + description));
            }
            if (auto valid = validateDocumentContent(payload["content"], description); !valid) {
                return std::unexpected(valid.error());
            }
        } else {
            return std::unexpected(corrupt("Unknown sidecar record type in " + description));
        }
        return value;
    }

private:
    Error corrupt(const std::string& message) const {
        return Error{"jsonl_corruption", message};
    }

    bool safeInteger(const Json& value) const {
        if (!value.is_number_integer()) {
            return false;
        }
        const std::int64_t number = value.get<std::int64_t>();
        return number >= -9007199254740991LL && number <= 9007199254740991LL;
    }

    Result<Json> parseJson(const std::string& text, const std::string& description) const {
        Json parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected(corrupt("Malformed complete " + description));
        }
        return parsed;
    }

    Result<void> validateMainOperation(Json value, const std::string& description) const {
        if (!value.is_object() || !value.value("type", Json()).is_string()) {
            return std::unexpected(corrupt("Invalid write in " + description));
        }
        const std::string type = value["type"].get<std::string>();
        if (type == "conversation" || type == "entry" || type == "submission") {
            if (!value["value"].is_object() || !safeInteger(value["value"]["id"])) {
                return std::unexpected(corrupt("Invalid " + type + " write in " + description));
            }
        } else if (type == "task") {
            Json& task = value["value"];
            if (!task.is_object() || !safeInteger(task["id"]) || !task["state"].is_object() ||
                task["state"].value("status", Json()) != "terminal") {
                return std::unexpected(corrupt("Invalid terminal task write in " + description));
            }
        } else if (type == "document.retire") {
            if (!safeInteger(value["id"])) {
                return std::unexpected(corrupt("Invalid document retirement in " + description));
            }
        } else if (type == "task.sidecar") {
            if (!safeInteger(value["id"]) || !safeInteger(value["ordinal"]) || value["ordinal"].get<std::int64_t>() < 0) {
                return std::unexpected(corrupt("Invalid task sidecar write in " + description));
            }
        } else if (type == "document.create") {
            if (!value["record"].is_object() || !safeInteger(value["record"]["id"]) || !safeInteger(value["ordinal"]) ||
                value["ordinal"].get<std::int64_t>() < 0) {
                return std::unexpected(corrupt("Invalid document creation in " + description));
            }
        } else if (type == "document.change") {
            if (!safeInteger(value["id"]) || !safeInteger(value["ordinal"]) || value["ordinal"].get<std::int64_t>() < 0) {
                return std::unexpected(corrupt("Invalid document change in " + description));
            }
        } else {
            return std::unexpected(corrupt("Unknown write type in " + description));
        }
        return {};
    }

    Result<void> validateDocumentContent(Json value, const std::string& description) const {
        if (!value.is_object() || !safeInteger(value["version"]) || value["version"].get<std::int64_t>() < 1) {
            return std::unexpected(corrupt("Invalid document content in " + description));
        }
        if ((value["kind"] == "base" && value["value"].is_object()) || (value["kind"] == "delta" && value["ops"].is_array())) {
            return {};
        }
        return std::unexpected(corrupt("Invalid document content in " + description));
    }
};
