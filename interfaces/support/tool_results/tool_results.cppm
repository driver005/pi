module;

#include <cstdint>

export module pi.support.tool_results;

import std;
export import pi.support.entry_kinds;
export import pi.support.transaction;
export import pi.support.usage_ledger;
export import pi.types.json;
export import pi.types.result;
export import pi.types.tool_diagnostic;
export import pi.types.tool_execution_result;

/**
 * Builds and appends tool results: the `pi.tool-result` entry whose message content ends with the rendered
 * diagnostics, so the stored message is exactly what the model sees. Port of the result helpers of tool.ts.
 */
export class ToolResults {
public:
    /** An error result the harness writes itself: no content and one `error` diagnostic with `code`. */
    ToolExecutionResult harnessError(const std::string& code, const std::string& message) const {
        ToolExecutionResult result;
        result.content = Json::array();
        result.isError = true;
        result.diagnostics.push_back(diagnostic("error", code, message));
        return result;
    }

    ToolDiagnostic diagnostic(const std::string& severity, const std::string& code, const std::string& message) const {
        ToolDiagnostic value;
        value.severity = severity;
        value.code = code;
        value.message = message;
        return value;
    }

    Json diagnosticJson(const ToolDiagnostic& diagnostic) const {
        Json json = Json::object({{"severity", diagnostic.severity}, {"message", diagnostic.message}});
        if (diagnostic.code) {
            json["code"] = *diagnostic.code;
        }
        return json;
    }

    ToolDiagnostic diagnosticFromJson(const Json& json) const {
        ToolDiagnostic value;
        value.severity = json.value("severity", std::string("info"));
        value.message = json.value("message", std::string());
        if (json.contains("code")) {
            value.code = json.at("code").get<std::string>();
        }
        return value;
    }

    /** A result as JSON, for hooks: `{content?, isError?, details?, diagnostics?, usage?, control?}`. */
    Json toJson(const ToolExecutionResult& result) const {
        Json json = Json::object();
        if (result.content) {
            json["content"] = *result.content;
        }
        if (result.isError) {
            json["isError"] = *result.isError;
        }
        if (result.details) {
            json["details"] = *result.details;
        }
        Json diagnostics = Json::array();
        for (const ToolDiagnostic& item : result.diagnostics) {
            diagnostics.push_back(diagnosticJson(item));
        }
        json["diagnostics"] = diagnostics;
        if (result.usage) {
            json["usage"] = *result.usage;
        }
        if (result.control) {
            json["control"] = *result.control;
        }
        return json;
    }

    ToolExecutionResult fromJson(const Json& json) const {
        ToolExecutionResult result;
        if (json.contains("content")) {
            result.content = json.at("content");
        }
        if (json.contains("isError")) {
            result.isError = json.at("isError").get<bool>();
        }
        if (json.contains("details")) {
            result.details = json.at("details");
        }
        if (json.contains("diagnostics")) {
            for (const Json& item : json.at("diagnostics")) {
                result.diagnostics.push_back(diagnosticFromJson(item));
            }
        }
        if (json.contains("usage")) {
            result.usage = json.at("usage");
        }
        if (json.contains("control")) {
            result.control = json.at("control");
        }
        return result;
    }

    std::string renderDiagnostics(const std::vector<ToolDiagnostic>& diagnostics) const {
        std::string text = "<harness>\n";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            text += (i == 0 ? "" : "\n") + std::string("[") + diagnostics[i].severity + "] " + diagnostics[i].message;
        }
        return text + "\n</harness>";
    }

    /**
     * Appends a `pi.tool-result` entry for `call` (`{id, name, ...}`). A result's usage is added to `pi.usage` in the
     * same commit. Returns the entry record.
     */
    Result<Json> append(Transaction& tx, std::int64_t conversationId, const Json& call, const ToolExecutionResult& result,
                        std::int64_t timestamp) const {
        Json content = result.content ? *result.content : Json::array();
        Json diagnostics = Json::array();
        for (const ToolDiagnostic& item : result.diagnostics) {
            diagnostics.push_back(diagnosticJson(item));
        }
        if (!result.diagnostics.empty()) {
            content.push_back(Json::object({{"type", "text"}, {"text", renderDiagnostics(result.diagnostics)}}));
        }
        Json message = Json::object({{"role", "toolResult"}, {"toolCallId", call.at("id")}, {"toolName", call.at("name")}, {"content", content}});
        if (result.details) {
            message["details"] = *result.details;
        }
        if (result.usage) {
            message["usage"] = *result.usage;
        }
        message["isError"] = result.isError.value_or(false);
        message["timestamp"] = timestamp;
        if (result.usage) {
            if (auto recorded = m_ledger.record(tx, conversationId, "tools", call.at("name").get<std::string>(), *result.usage); !recorded) {
                return std::unexpected(recorded.error());
            }
        }
        return tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.toolResult()},
                                                            {"model", Json::array({message})},
                                                            {"data", Json::object({{"diagnostics", diagnostics}})}}));
    }

private:
    EntryKinds m_kinds;
    UsageLedger m_ledger;
};
