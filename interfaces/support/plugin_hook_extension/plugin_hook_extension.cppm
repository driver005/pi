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
 * observations; `beforeProviderRequest` -> `before_provider_request` {payload} with any non-null result replacing the
 * payload; `beforeCompact` -> `session_before_compact` {preparation: {firstKeptEntryId, messagesToSummarize}, branchEntries
 * (the entries before the cut), customInstructions?, reason, willRetry: false}, where `{cancel}` declines the compaction and
 * `{compaction: {summary}}` places that summary instead of asking the model (the durable task keeps the cut it chose, so
 * `firstKeptEntryId` of a plugin's result is ignored). A handler's `terminate` flag is not used: the durable tool task has no equivalent. Without subscribers every
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
        generation.handlers["beforeProviderRequest"] = [self = *this](const Json& payload, IHookApi&) { return self.beforeProviderRequest(payload); };
        generation.handlers["afterResponse"] = [self = *this](const Json& message, IHookApi&) { return self.observe("message_end", Json::object({{"message", message}})); };
        generation.handlers["afterTools"] = [self = *this](const Json& payload, IHookApi&) {
            return self.observe("turn_end", Json::object({{"message", payload.value("assistant", Json())}, {"toolResults", payload.value("results", Json::array())}}));
        };
        extension.hooks.push_back(std::move(generation));
        HookRegistration compaction;
        compaction.task = "pi.compaction";
        compaction.handlers["beforeCompact"] = [self = *this](const Json& compaction, IHookApi&) { return self.beforeCompact(compaction); };
        extension.hooks.push_back(std::move(compaction));
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

    Result<std::optional<Json>> beforeProviderRequest(const Json& payload) const {
        if (!m_bus->hasHandlers("before_provider_request")) {
            return std::optional<Json>();
        }
        bool replaced = false;
        const HookOutcome outcome = m_bus->emit("before_provider_request", payload, [&replaced](Json& current, const Json& answer) {
            if (!answer.is_null()) {
                current["payload"] = answer;
                replaced = true;
            }
        });
        if (!replaced) {
            return std::optional<Json>();
        }
        return std::optional<Json>(Json::object({{"payload", outcome.payload.at("payload")}}));
    }

    Result<std::optional<Json>> beforeCompact(const Json& compaction) const {
        if (!m_bus->hasHandlers("session_before_compact")) {
            return std::optional<Json>();
        }
        Json event = Json::object({{"preparation", Json::object({{"firstKeptEntryId", compaction.value("firstKept", Json())},
                                                                  {"messagesToSummarize", compaction.value("messages", Json::array())}})},
                                   {"branchEntries", compaction.value("entries", Json::array())},
                                   {"reason", compaction.value("reason", Json("manual"))},
                                   {"willRetry", false}});
        if (compaction.contains("instructions")) {
            event["customInstructions"] = compaction["instructions"];
        }
        const HookOutcome outcome = m_bus->emit("session_before_compact", event);
        std::optional<std::string> summary;
        for (const Json& answer : outcome.results) {
            if (!answer.is_object()) {
                continue;
            }
            if (answer.value("cancel", false)) {
                return std::optional<Json>(Json::object({{"decline", true}}));
            }
            const Json supplied = answer.value("compaction", Json());
            if (supplied.is_object() && supplied.contains("summary") && supplied["summary"].is_string()) {
                summary = supplied["summary"].get<std::string>();
            }
        }
        if (summary) {
            return std::optional<Json>(Json::object({{"summary", *summary}}));
        }
        return std::optional<Json>();
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
