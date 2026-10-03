export module pi.support.plugin_hook_extension;

import std;
export import pi.plugin.i_hook_bus;
export import pi.types.extension;

/**
 * Offers the plugin hooks of a directory to the durable harness as an extension. The extension's hooks answer the durable
 * hook points with the events plugins subscribe to (see PluginHookDispatcher for the non-durable counterpart):
 * `beforeTool` -> `tool_call` {toolCallId, toolName, input}; a handler result `{block, reason?}` blocks the call with that
 * reason and `{input}` replaces the arguments (later handlers see the replacement); `afterTool` -> `tool_result`
 * {toolCallId, toolName, input, content, details, isError} with `{content?, details?, isError?}` results replacing parts of
 * the result; `beforeRequest` -> `context` {messages} with `{messages}` results replacing the request's messages;
 * `afterResponse` -> `message_end` {type, message} and `afterTools` -> `turn_end` {type, message, toolResults}, which are
 * observations. A handler's `terminate` flag is not used: the durable tool task has no equivalent. Without subscribers every
 * hook is a no-op, so the extension is installed unconditionally. The bus is held by shared ownership, so a registry can
 * outlive the plugins (their handlers are unsubscribed when they shut down).
 */
export class PluginHookExtension {
public:
    explicit PluginHookExtension(std::shared_ptr<IHookBus> bus)
        : m_bus(std::move(bus)) {}

    Extension extension(const std::string& name) const {
        Extension extension;
        extension.name = name;
        HookRegistration tool;
        tool.task = "pi.tool";
        tool.handlers["beforeTool"] = [self = *this](const Json& call, IHookApi&) { return self.beforeTool(call); };
        tool.handlers["afterTool"] = [self = *this](const Json& payload, IHookApi&) { return self.afterTool(payload); };
        extension.hooks.push_back(std::move(tool));
        HookRegistration generation;
        generation.task = "pi.generation";
        generation.handlers["beforeRequest"] = [self = *this](const Json& payload, IHookApi&) { return self.beforeRequest(payload); };
        generation.handlers["afterResponse"] = [self = *this](const Json& message, IHookApi&) { return self.observe("message_end", Json::object({{"message", message}})); };
        generation.handlers["afterTools"] = [self = *this](const Json& payload, IHookApi&) {
            return self.observe("turn_end", Json::object({{"message", payload.value("assistant", Json())}, {"toolResults", payload.value("results", Json::array())}}));
        };
        extension.hooks.push_back(std::move(generation));
        return extension;
    }

private:
    Result<std::optional<Json>> beforeTool(const Json& call) const {
        if (!m_bus->hasHandlers("tool_call")) {
            return std::optional<Json>();
        }
        const Json original = call.value("arguments", Json::object());
        const Json payload = Json::object({{"toolCallId", call.value("id", Json())}, {"toolName", call.value("name", Json())}, {"input", original}});
        const HookOutcome outcome = m_bus->emit("tool_call", payload, [](Json& current, const Json& result) {
            if (result.is_object() && result.contains("input") && result["input"].is_object()) {
                current["input"] = result["input"];
            }
        });
        for (const Json& entry : outcome.results) {
            if (entry.is_object() && entry.value("block", false)) {
                const Json reason = entry.value("reason", Json());
                return std::optional<Json>(Json::object({{"block", reason.is_string() ? reason : Json("Blocked by a plugin")}}));
            }
        }
        if (outcome.payload.at("input") != original) {
            return std::optional<Json>(Json::object({{"arguments", outcome.payload.at("input")}}));
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> afterTool(const Json& payload) const {
        if (!m_bus->hasHandlers("tool_result")) {
            return std::optional<Json>();
        }
        const Json call = payload.value("call", Json::object());
        const Json result = payload.value("result", Json::object());
        const Json event = Json::object({{"toolCallId", call.value("id", Json())},
                                         {"toolName", call.value("name", Json())},
                                         {"input", call.value("arguments", Json::object())},
                                         {"content", result.value("content", Json::array())},
                                         {"details", result.value("details", Json())},
                                         {"isError", result.value("isError", false)}});
        const HookOutcome outcome = m_bus->emit("tool_result", event, [](Json& current, const Json& answer) {
            if (!answer.is_object()) {
                return;
            }
            for (const char* key : {"content", "details", "isError"}) {
                if (answer.contains(key)) {
                    current[key] = answer[key];
                }
            }
        });
        bool changed = false;
        Json replaced = result;
        for (const Json& entry : outcome.results) {
            if (!entry.is_object()) {
                continue;
            }
            for (const char* key : {"content", "details", "isError"}) {
                if (entry.contains(key)) {
                    replaced[key] = outcome.payload.at(key);
                    changed = true;
                }
            }
        }
        return changed ? std::optional<Json>(replaced) : std::optional<Json>();
    }

    Result<std::optional<Json>> beforeRequest(const Json& payload) const {
        if (!m_bus->hasHandlers("context")) {
            return std::optional<Json>();
        }
        const HookOutcome outcome = m_bus->emit("context", payload, [](Json& current, const Json& answer) {
            if (answer.is_object() && answer.contains("messages") && answer["messages"].is_array()) {
                current["messages"] = answer["messages"];
            }
        });
        if (outcome.results.empty()) {
            return std::optional<Json>();
        }
        return std::optional<Json>(Json::object({{"messages", outcome.payload.at("messages")}}));
    }

    Result<std::optional<Json>> observe(const std::string& type, Json event) const {
        if (m_bus->hasHandlers(type)) {
            event["type"] = type;
            m_bus->emit(type, event);
        }
        return std::optional<Json>();
    }

    std::shared_ptr<IHookBus> m_bus;
};
