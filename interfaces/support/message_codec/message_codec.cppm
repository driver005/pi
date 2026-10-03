export module pi.support.message_codec;

import std;
export import pi.support.json_reader;
export import pi.types.assistant_content_block;
export import pi.types.assistant_message;
export import pi.types.deferred_handle;
export import pi.types.json;
export import pi.types.message;
export import pi.types.nested_tool_calls;
export import pi.types.result;
export import pi.types.stop_reason;
export import pi.types.system_message;
export import pi.types.thinking_level;
export import pi.types.tool;
export import pi.types.tool_result_message;
export import pi.types.usage;
export import pi.types.user_content_block;
export import pi.types.user_message;

/**
 * JSON <-> C++ conversion for transcript messages. The JSON shape is the one pi has always
 * written to session files and the wire (role-tagged messages, type-tagged content blocks,
 * optional fields omitted when unset), so TypeScript and C++ read each other's data.
 */
export class MessageCodec {
public:
    std::string stopReasonName(StopReason reason) const {
        switch (reason) {
            case StopReason::Pending: return "pending";
            case StopReason::Stop: return "stop";
            case StopReason::Length: return "length";
            case StopReason::ToolUse: return "toolUse";
            case StopReason::Error: return "error";
            case StopReason::Aborted: return "aborted";
            case StopReason::Deferred: return "deferred";
        }
        return "stop";
    }

    std::optional<StopReason> parseStopReason(const std::string& name) const {
        for (const StopReason reason : {StopReason::Pending, StopReason::Stop, StopReason::Length,
                                        StopReason::ToolUse, StopReason::Error, StopReason::Aborted,
                                        StopReason::Deferred}) {
            if (stopReasonName(reason) == name) {
                return reason;
            }
        }
        return std::nullopt;
    }

    std::string thinkingLevelName(ThinkingLevel level) const {
        switch (level) {
            case ThinkingLevel::Off: return "off";
            case ThinkingLevel::Minimal: return "minimal";
            case ThinkingLevel::Low: return "low";
            case ThinkingLevel::Medium: return "medium";
            case ThinkingLevel::High: return "high";
            case ThinkingLevel::XHigh: return "xhigh";
            case ThinkingLevel::Max: return "max";
        }
        return "off";
    }

    std::optional<ThinkingLevel> parseThinkingLevel(const std::string& name) const {
        for (const ThinkingLevel level : {ThinkingLevel::Off, ThinkingLevel::Minimal, ThinkingLevel::Low,
                                          ThinkingLevel::Medium, ThinkingLevel::High, ThinkingLevel::XHigh,
                                          ThinkingLevel::Max}) {
            if (thinkingLevelName(level) == name) {
                return level;
            }
        }
        return std::nullopt;
    }

    Json toJson(const TextContent& block) const {
        Json json = {{"type", "text"}, {"text", block.text}};
        setOptional(json, "textSignature", block.textSignature);
        return json;
    }

    Json toJson(const ThinkingContent& block) const {
        Json json = {{"type", "thinking"}, {"thinking", block.thinking}};
        setOptional(json, "thinkingSignature", block.thinkingSignature);
        if (block.redacted.has_value()) {
            json["redacted"] = *block.redacted;
        }
        return json;
    }

    Json toJson(const ImageContent& block) const {
        return {{"type", "image"}, {"data", block.data}, {"mimeType", block.mimeType}};
    }

    Json toJson(const ToolCall& call) const {
        Json json = {{"type", "toolCall"}, {"id", call.id}, {"name", call.name}, {"arguments", call.arguments}};
        setOptional(json, "thoughtSignature", call.thoughtSignature);
        setOptional(json, "namespace", call.toolNamespace);
        return json;
    }

    Json toJson(const UserContentBlock& block) const {
        return std::visit([this](const auto& value) { return toJson(value); }, block);
    }

    Json toJson(const AssistantContentBlock& block) const {
        return std::visit([this](const auto& value) { return toJson(value); }, block);
    }

    Json toJson(const Usage& usage) const {
        Json json = {{"input", usage.input},
                     {"output", usage.output},
                     {"cacheRead", usage.cacheRead},
                     {"cacheWrite", usage.cacheWrite}};
        if (usage.cacheWrite1h.has_value()) {
            json["cacheWrite1h"] = *usage.cacheWrite1h;
        }
        if (usage.reasoning.has_value()) {
            json["reasoning"] = *usage.reasoning;
        }
        json["totalTokens"] = usage.totalTokens;
        json["cost"] = {{"input", usage.cost.input},
                        {"output", usage.cost.output},
                        {"cacheRead", usage.cost.cacheRead},
                        {"cacheWrite", usage.cost.cacheWrite},
                        {"total", usage.cost.total}};
        return json;
    }

    Json toJson(const Tool& tool) const {
        Json json = {{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}};
        if (!tool.constrainedSampling.is_null()) {
            json["constrainedSampling"] = tool.constrainedSampling;
        }
        return json;
    }

    Json toJson(const DeferredHandle& handle) const {
        Json json = {{"provider", handle.provider},
                     {"modelId", handle.modelId},
                     {"api", handle.api},
                     {"id", handle.id}};
        if (handle.expiresAt.has_value()) {
            json["expiresAt"] = *handle.expiresAt;
        }
        if (handle.pollAfterMs.has_value()) {
            json["pollAfterMs"] = *handle.pollAfterMs;
        }
        if (!handle.data.is_null()) {
            json["data"] = handle.data;
        }
        return json;
    }

    Json toJson(const NestedToolCalls& nested) const {
        Json calls = Json::array();
        for (const NestedToolCallRecord& record : nested.calls) {
            Json entry = {{"id", record.id}, {"name", record.name}, {"status", record.status}};
            if (!record.arguments.is_null()) {
                entry["arguments"] = record.arguments;
            }
            if (record.argumentsBytes.has_value()) {
                entry["argumentsBytes"] = *record.argumentsBytes;
            }
            if (record.durationMs.has_value()) {
                entry["durationMs"] = *record.durationMs;
            }
            setOptional(entry, "error", record.error);
            calls.push_back(std::move(entry));
        }
        return {{"calls", std::move(calls)}, {"complete", nested.complete}};
    }

    Json toJson(const SystemMessage& message) const {
        Json json = {{"role", "system"}};
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            json["content"] = *text;
        } else {
            Json blocks = Json::array();
            for (const TextContent& block : std::get<std::vector<TextContent>>(message.content)) {
                blocks.push_back(toJson(block));
            }
            json["content"] = std::move(blocks);
        }
        if (message.sections.has_value()) {
            Json sections = Json::object();
            for (const auto& [name, value] : *message.sections) {
                sections[name] = value.has_value() ? Json(*value) : Json(nullptr);
            }
            json["sections"] = std::move(sections);
        }
        if (message.toolsAdded.has_value()) {
            Json tools = Json::array();
            for (const Tool& tool : *message.toolsAdded) {
                tools.push_back(toJson(tool));
            }
            json["toolsAdded"] = std::move(tools);
        }
        if (message.toolsRemoved.has_value()) {
            Json removed = Json::array();
            for (const ToolReference& reference : *message.toolsRemoved) {
                removed.push_back({{"name", reference.name}});
            }
            json["toolsRemoved"] = std::move(removed);
        }
        json["timestamp"] = message.timestamp;
        return json;
    }

    Json toJson(const UserMessage& message) const {
        Json json = {{"role", "user"}};
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            json["content"] = *text;
        } else {
            json["content"] = userContentToJson(std::get<std::vector<UserContentBlock>>(message.content));
        }
        json["timestamp"] = message.timestamp;
        return json;
    }

    Json toJson(const AssistantMessage& message) const {
        Json content = Json::array();
        for (const AssistantContentBlock& block : message.content) {
            content.push_back(toJson(block));
        }
        Json json = {{"role", "assistant"},
                     {"content", std::move(content)},
                     {"api", message.api},
                     {"provider", message.provider},
                     {"model", message.model}};
        setOptional(json, "responseModel", message.responseModel);
        setOptional(json, "responseId", message.responseId);
        setOptional(json, "providerThinkingLevel", message.providerThinkingLevel);
        if (message.thinkingLevel.has_value()) {
            json["thinkingLevel"] = thinkingLevelName(*message.thinkingLevel);
        }
        if (!message.diagnostics.is_null()) {
            json["diagnostics"] = message.diagnostics;
        }
        json["usage"] = toJson(message.usage);
        json["stopReason"] = stopReasonName(message.stopReason);
        if (message.deferred.has_value()) {
            json["deferred"] = toJson(*message.deferred);
        }
        setOptional(json, "errorMessage", message.errorMessage);
        setOptional(json, "rawStopReason", message.rawStopReason);
        if (message.endTurn.has_value()) {
            json["endTurn"] = *message.endTurn;
        }
        json["timestamp"] = message.timestamp;
        return json;
    }

    Json toJson(const ToolResultMessage& message) const {
        Json json = {{"role", "toolResult"},
                     {"toolCallId", message.toolCallId},
                     {"toolName", message.toolName},
                     {"content", userContentToJson(message.content)}};
        if (!message.details.is_null()) {
            json["details"] = message.details;
        }
        if (message.usage.has_value()) {
            json["usage"] = toJson(*message.usage);
        }
        if (message.nestedCalls.has_value()) {
            json["nestedCalls"] = toJson(*message.nestedCalls);
        }
        json["isError"] = message.isError;
        json["timestamp"] = message.timestamp;
        return json;
    }

    Json toJson(const Message& message) const {
        return std::visit([this](const auto& value) { return toJson(value); }, message);
    }

    Json toJson(const std::vector<Message>& messages) const {
        Json array = Json::array();
        for (const Message& message : messages) {
            array.push_back(toJson(message));
        }
        return array;
    }

    Result<UserContentBlock> userBlockFromJson(const Json& json) const {
        const JsonReader reader(json);
        const auto type = reader.requireString("type");
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type == "text") {
            const auto text = reader.requireString("text");
            if (!text) {
                return std::unexpected(text.error());
            }
            return UserContentBlock(TextContent{*text, reader.optString("textSignature")});
        }
        if (*type == "image") {
            const auto data = reader.requireString("data");
            const auto mime = reader.requireString("mimeType");
            if (!data || !mime) {
                return std::unexpected(!data ? data.error() : mime.error());
            }
            return UserContentBlock(ImageContent{*data, *mime});
        }
        return std::unexpected(Error{"invalid_json", "unknown content block type: " + *type});
    }

    Result<AssistantContentBlock> assistantBlockFromJson(const Json& json) const {
        const JsonReader reader(json);
        const auto type = reader.requireString("type");
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type == "thinking") {
            const auto thinking = reader.requireString("thinking");
            if (!thinking) {
                return std::unexpected(thinking.error());
            }
            return AssistantContentBlock(
                ThinkingContent{*thinking, reader.optString("thinkingSignature"), reader.optBool("redacted")});
        }
        if (*type == "toolCall") {
            auto call = toolCallFromJson(json);
            if (!call) {
                return std::unexpected(call.error());
            }
            return AssistantContentBlock(std::move(*call));
        }
        auto user = userBlockFromJson(json);
        if (!user || !std::holds_alternative<TextContent>(*user)) {
            return std::unexpected(Error{"invalid_json", "unknown assistant content block type: " + *type});
        }
        return AssistantContentBlock(std::get<TextContent>(*user));
    }

    Result<ToolCall> toolCallFromJson(const Json& json) const {
        const JsonReader reader(json);
        const auto id = reader.requireString("id");
        const auto name = reader.requireString("name");
        if (!id || !name) {
            return std::unexpected(!id ? id.error() : name.error());
        }
        ToolCall call;
        call.id = *id;
        call.name = *name;
        call.arguments = reader.get("arguments").is_object() ? reader.get("arguments") : Json::object();
        call.thoughtSignature = reader.optString("thoughtSignature");
        call.toolNamespace = reader.optString("namespace");
        return call;
    }

    Result<Usage> usageFromJson(const Json& json) const {
        const JsonReader reader(json, "usage");
        Usage usage;
        usage.input = reader.optInt("input").value_or(0);
        usage.output = reader.optInt("output").value_or(0);
        usage.cacheRead = reader.optInt("cacheRead").value_or(0);
        usage.cacheWrite = reader.optInt("cacheWrite").value_or(0);
        usage.cacheWrite1h = reader.optInt("cacheWrite1h");
        usage.reasoning = reader.optInt("reasoning");
        usage.totalTokens = reader.optInt("totalTokens")
                                .value_or(usage.input + usage.output + usage.cacheRead + usage.cacheWrite);
        const JsonReader cost = reader.child("cost");
        usage.cost.input = cost.optNumber("input").value_or(0);
        usage.cost.output = cost.optNumber("output").value_or(0);
        usage.cost.cacheRead = cost.optNumber("cacheRead").value_or(0);
        usage.cost.cacheWrite = cost.optNumber("cacheWrite").value_or(0);
        usage.cost.total = cost.optNumber("total").value_or(0);
        return usage;
    }

    Result<Tool> toolFromJson(const Json& json) const {
        const JsonReader reader(json);
        const auto name = reader.requireString("name");
        if (!name) {
            return std::unexpected(name.error());
        }
        Tool tool;
        tool.name = *name;
        tool.description = reader.optString("description").value_or("");
        tool.parameters = reader.get("parameters").is_null() ? Json::object() : reader.get("parameters");
        tool.constrainedSampling = reader.get("constrainedSampling");
        return tool;
    }

    Result<Message> messageFromJson(const Json& json) const {
        const JsonReader reader(json);
        const auto role = reader.requireString("role");
        if (!role) {
            return std::unexpected(role.error());
        }
        if (*role == "system") {
            auto message = systemFromJson(reader);
            return message ? Result<Message>(Message(std::move(*message))) : std::unexpected(message.error());
        }
        if (*role == "user") {
            auto message = userFromJson(reader);
            return message ? Result<Message>(Message(std::move(*message))) : std::unexpected(message.error());
        }
        if (*role == "assistant") {
            auto message = assistantMessageFromJson(json);
            return message ? Result<Message>(Message(std::move(*message))) : std::unexpected(message.error());
        }
        if (*role == "toolResult") {
            auto message = toolResultFromJson(reader);
            return message ? Result<Message>(Message(std::move(*message))) : std::unexpected(message.error());
        }
        return std::unexpected(Error{"invalid_json", "unknown message role: " + *role});
    }

    Result<AssistantMessage> assistantMessageFromJson(const Json& json) const {
        const JsonReader reader(json, "assistant");
        AssistantMessage message;
        const auto blocks = reader.requireArray("content");
        if (!blocks) {
            return std::unexpected(blocks.error());
        }
        for (const Json& entry : **blocks) {
            auto block = assistantBlockFromJson(entry);
            if (!block) {
                return std::unexpected(block.error());
            }
            message.content.push_back(std::move(*block));
        }
        message.api = reader.optString("api").value_or("");
        message.provider = reader.optString("provider").value_or("");
        message.model = reader.optString("model").value_or("");
        message.responseModel = reader.optString("responseModel");
        message.responseId = reader.optString("responseId");
        message.providerThinkingLevel = reader.optString("providerThinkingLevel");
        if (const auto level = reader.optString("thinkingLevel"); level.has_value()) {
            message.thinkingLevel = parseThinkingLevel(*level);
        }
        message.diagnostics = reader.get("diagnostics");
        auto usage = usageFromJson(reader.get("usage"));
        if (!usage) {
            return std::unexpected(usage.error());
        }
        message.usage = *usage;
        const auto stop = parseStopReason(reader.optString("stopReason").value_or("stop"));
        message.stopReason = stop.value_or(StopReason::Stop);
        message.deferred = deferredFromJson(reader.get("deferred"));
        message.errorMessage = reader.optString("errorMessage");
        message.rawStopReason = reader.optString("rawStopReason");
        message.endTurn = reader.optBool("endTurn");
        message.timestamp = reader.optInt("timestamp").value_or(0);
        return message;
    }

    Result<std::vector<Message>> messagesFromJson(const Json& json) const {
        if (!json.is_array()) {
            return std::unexpected(Error{"invalid_json", "messages: expected array"});
        }
        std::vector<Message> messages;
        for (const Json& entry : json) {
            auto message = messageFromJson(entry);
            if (!message) {
                return std::unexpected(message.error());
            }
            messages.push_back(std::move(*message));
        }
        return messages;
    }

private:
    Result<SystemMessage> systemFromJson(const JsonReader& reader) const {
        SystemMessage message;
        const Json& content = reader.get("content");
        if (content.is_string()) {
            message.content = content.get<std::string>();
        } else if (content.is_array()) {
            auto blocks = textBlocksFromJson(content, reader.pathOf("content"));
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            message.content = std::move(*blocks);
        }
        if (reader.get("sections").is_object()) {
            std::vector<std::pair<std::string, std::optional<std::string>>> sections;
            for (const auto& entry : reader.get("sections").items()) {
                const std::string& name = entry.key();
                const Json& value = entry.value();
                sections.emplace_back(name, value.is_string() ? std::optional<std::string>(value.get<std::string>())
                                                              : std::nullopt);
            }
            message.sections = std::move(sections);
        }
        if (reader.get("toolsAdded").is_array()) {
            auto tools = toolsFromJson(reader.get("toolsAdded"), reader.pathOf("toolsAdded"));
            if (!tools) {
                return std::unexpected(tools.error());
            }
            message.toolsAdded = std::move(*tools);
        }
        if (reader.get("toolsRemoved").is_array()) {
            std::vector<ToolReference> removed;
            for (const Json& entry : reader.get("toolsRemoved")) {
                removed.push_back({JsonReader(entry).optString("name").value_or("")});
            }
            message.toolsRemoved = std::move(removed);
        }
        message.timestamp = reader.optInt("timestamp").value_or(0);
        return message;
    }

    Result<UserMessage> userFromJson(const JsonReader& reader) const {
        UserMessage message;
        const Json& content = reader.get("content");
        if (content.is_string()) {
            message.content = content.get<std::string>();
        } else {
            auto blocks = userBlocksFromJson(content, reader.pathOf("content"));
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            message.content = std::move(*blocks);
        }
        message.timestamp = reader.optInt("timestamp").value_or(0);
        return message;
    }

    Result<ToolResultMessage> toolResultFromJson(const JsonReader& reader) const {
        ToolResultMessage message;
        const auto id = reader.requireString("toolCallId");
        if (!id) {
            return std::unexpected(id.error());
        }
        message.toolCallId = *id;
        message.toolName = reader.optString("toolName").value_or("");
        auto blocks = userBlocksFromJson(reader.get("content").is_array() ? reader.get("content") : Json::array(),
                                         reader.pathOf("content"));
        if (!blocks) {
            return std::unexpected(blocks.error());
        }
        message.content = std::move(*blocks);
        message.details = reader.get("details");
        if (reader.get("usage").is_object()) {
            auto usage = usageFromJson(reader.get("usage"));
            if (usage) {
                message.usage = *usage;
            }
        }
        auto nested = nestedFromJson(reader);
        if (nested) {
            message.nestedCalls = std::move(*nested);
        }
        message.isError = reader.optBool("isError").value_or(false);
        message.timestamp = reader.optInt("timestamp").value_or(0);
        return message;
    }

    Result<std::vector<UserContentBlock>> userBlocksFromJson(const Json& array, const std::string& path) const {
        std::vector<UserContentBlock> blocks;
        if (!array.is_array()) {
            return std::unexpected(Error{"invalid_json", path + ": expected array"});
        }
        for (const Json& entry : array) {
            auto block = userBlockFromJson(entry);
            if (!block) {
                return std::unexpected(block.error());
            }
            blocks.push_back(std::move(*block));
        }
        return blocks;
    }

    Result<std::vector<TextContent>> textBlocksFromJson(const Json& array, const std::string& path) const {
        auto blocks = userBlocksFromJson(array, path);
        if (!blocks) {
            return std::unexpected(blocks.error());
        }
        std::vector<TextContent> texts;
        for (const UserContentBlock& block : *blocks) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                texts.push_back(*text);
            }
        }
        return texts;
    }

    Result<std::vector<Tool>> toolsFromJson(const Json& array, const std::string& path) const {
        std::vector<Tool> tools;
        if (!array.is_array()) {
            return std::unexpected(Error{"invalid_json", path + ": expected array"});
        }
        for (const Json& entry : array) {
            auto tool = toolFromJson(entry);
            if (!tool) {
                return std::unexpected(tool.error());
            }
            tools.push_back(std::move(*tool));
        }
        return tools;
    }

    Result<std::optional<NestedToolCalls>> nestedFromJson(const JsonReader& reader) const {
        if (!reader.get("nestedCalls").is_object()) {
            return std::optional<NestedToolCalls>();
        }
        const JsonReader nestedReader = reader.child("nestedCalls");
        NestedToolCalls nested;
        nested.complete = nestedReader.optBool("complete").value_or(true);
        for (const Json& entry : nestedReader.get("calls")) {
            const JsonReader record(entry);
            NestedToolCallRecord item;
            item.id = record.optString("id").value_or("");
            item.name = record.optString("name").value_or("");
            item.arguments = record.get("arguments");
            item.argumentsBytes = record.optInt("argumentsBytes");
            item.status = record.optString("status").value_or("ok");
            item.durationMs = record.optInt("durationMs");
            item.error = record.optString("error");
            nested.calls.push_back(std::move(item));
        }
        return std::optional<NestedToolCalls>(std::move(nested));
    }

    std::optional<DeferredHandle> deferredFromJson(const Json& json) const {
        const JsonReader reader(json);
        if (!reader.isObject()) {
            return std::nullopt;
        }
        DeferredHandle handle;
        handle.provider = reader.optString("provider").value_or("");
        handle.modelId = reader.optString("modelId").value_or("");
        handle.api = reader.optString("api").value_or("");
        handle.id = reader.optString("id").value_or("");
        handle.expiresAt = reader.optInt("expiresAt");
        handle.pollAfterMs = reader.optInt("pollAfterMs");
        handle.data = reader.get("data");
        return handle;
    }

    Json userContentToJson(const std::vector<UserContentBlock>& blocks) const {
        Json array = Json::array();
        for (const UserContentBlock& block : blocks) {
            array.push_back(toJson(block));
        }
        return array;
    }

    void setOptional(Json& object, const char* key, const std::optional<std::string>& value) const {
        if (value.has_value()) {
            object[key] = *value;
        }
    }
};
