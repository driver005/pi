export module pi.support.plugin_hook_dispatcher;

import std;
export import pi.plugin.i_hook_bus;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.types.after_tool_call_result;
export import pi.types.agent_message;
export import pi.types.before_tool_call_result;
export import pi.types.tool_call_context;

/**
 * Turns the agent loop's hook points into plugin events and plugin answers back into hook results.
 * Events (payload -> result): `tool_call` {toolCallId, toolName, input} -> {block?, reason?, terminate?,
 * input?}; `tool_result` {toolCallId, toolName, input, content, details, structuredContent, isError} ->
 * {content?, details?, structuredContent?, isError?}; `context` {messages} -> {messages?}. Handlers see
 * the changes of earlier handlers. Without subscribers every method is a no-op.
 */
export class PluginHookDispatcher {
public:
    explicit PluginHookDispatcher(IHookBus& bus);

    std::optional<BeforeToolCallResult> beforeToolCall(const ToolCallContext& context);
    std::optional<AfterToolCallResult> afterToolCall(const ToolCallContext& context);
    /** The messages after every `context` handler; the input when nobody changed them. */
    std::vector<AgentMessage> transformContext(const std::vector<AgentMessage>& messages);
    /** Fires an observation event named by json["type"] (agent_start, message_end, ...). */
    void notify(const Json& event);

private:
    Json contentJson(const std::vector<UserContentBlock>& blocks) const;
    std::optional<std::vector<UserContentBlock>> contentFrom(const Json& json) const;

    IHookBus& m_bus;
    MessageCodec m_messages;
    AgentMessageCodec m_agentMessages;
};

PluginHookDispatcher::PluginHookDispatcher(IHookBus& bus) : m_bus(bus) {}

Json PluginHookDispatcher::contentJson(const std::vector<UserContentBlock>& blocks) const {
    Json out = Json::array();
    for (const UserContentBlock& block : blocks) {
        out.push_back(m_messages.toJson(block));
    }
    return out;
}

std::optional<std::vector<UserContentBlock>> PluginHookDispatcher::contentFrom(const Json& json) const {
    if (!json.is_array()) {
        return std::nullopt;
    }
    std::vector<UserContentBlock> blocks;
    for (const Json& entry : json) {
        auto block = m_messages.userBlockFromJson(entry);
        if (!block) {
            return std::nullopt;
        }
        blocks.push_back(std::move(*block));
    }
    return blocks;
}

std::optional<BeforeToolCallResult> PluginHookDispatcher::beforeToolCall(const ToolCallContext& context) {
    if (!m_bus.hasHandlers("tool_call") || context.toolCall == nullptr) {
        return std::nullopt;
    }
    const Json original = context.args;
    const Json payload{{"toolCallId", context.toolCall->id}, {"toolName", context.toolCall->name}, {"input", original}};
    const HookOutcome outcome = m_bus.emit("tool_call", payload, [](Json& current, const Json& result) {
        if (result.is_object() && result.contains("input") && result["input"].is_object()) {
            current["input"] = result["input"];
        }
    });
    BeforeToolCallResult result;
    for (const Json& entry : outcome.results) {
        if (!entry.is_object()) {
            continue;
        }
        if (entry.value("block", false) && !result.block) {
            result.block = true;
            if (entry.contains("reason") && entry["reason"].is_string()) {
                result.reason = entry["reason"].get<std::string>();
            }
        }
        result.terminate = result.terminate || entry.value("terminate", false);
    }
    if (outcome.payload["input"] != original) {
        result.args = outcome.payload["input"];
    }
    if (!result.block && result.args.is_null()) {
        return std::nullopt;
    }
    return result;
}

std::optional<AfterToolCallResult> PluginHookDispatcher::afterToolCall(const ToolCallContext& context) {
    if (!m_bus.hasHandlers("tool_result") || context.toolCall == nullptr || !context.result) {
        return std::nullopt;
    }
    Json payload{{"toolCallId", context.toolCall->id},
                 {"toolName", context.toolCall->name},
                 {"input", context.args},
                 {"content", contentJson(context.result->content)},
                 {"details", context.result->details},
                 {"structuredContent", context.result->structuredContent},
                 {"isError", context.isError}};
    const HookOutcome outcome = m_bus.emit("tool_result", payload, [](Json& current, const Json& result) {
        if (!result.is_object()) {
            return;
        }
        for (const char* key : {"content", "details", "structuredContent", "isError"}) {
            if (result.contains(key)) {
                current[key] = result[key];
            }
        }
        if (result.contains("content") && !result.contains("structuredContent")) {
            current["structuredContent"] = Json();
        }
    });
    AfterToolCallResult result;
    bool changed = false;
    for (const Json& entry : outcome.results) {
        if (!entry.is_object()) {
            continue;
        }
        if (entry.contains("content")) {
            if (const auto blocks = contentFrom(outcome.payload["content"])) {
                result.content = *blocks;
                changed = true;
            }
        }
        changed = changed || entry.contains("details") || entry.contains("structuredContent") || entry.contains("isError");
    }
    if (!changed) {
        return std::nullopt;
    }
    result.details = outcome.payload["details"] == context.result->details ? Json() : outcome.payload["details"];
    result.structuredContent = outcome.payload["structuredContent"] == context.result->structuredContent
                                   ? Json()
                                   : outcome.payload["structuredContent"];
    if (outcome.payload["isError"].is_boolean() && outcome.payload["isError"].get<bool>() != context.isError) {
        result.isError = outcome.payload["isError"].get<bool>();
    }
    return result;
}

std::vector<AgentMessage> PluginHookDispatcher::transformContext(const std::vector<AgentMessage>& messages) {
    if (!m_bus.hasHandlers("context")) {
        return messages;
    }
    const Json payload{{"messages", m_agentMessages.listToJson(messages)}};
    const HookOutcome outcome = m_bus.emit("context", payload, [](Json& current, const Json& result) {
        if (result.is_object() && result.contains("messages") && result["messages"].is_array()) {
            current["messages"] = result["messages"];
        }
    });
    if (outcome.results.empty()) {
        return messages;
    }
    auto replaced = m_agentMessages.listFromJson(outcome.payload["messages"]);
    return replaced ? std::move(*replaced) : messages;
}

void PluginHookDispatcher::notify(const Json& event) {
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
        return;
    }
    const std::string name = event["type"].get<std::string>();
    if (m_bus.hasHandlers(name)) {
        m_bus.emit(name, event);
    }
}
