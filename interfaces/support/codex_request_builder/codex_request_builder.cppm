module;

#include <cstdint>

export module pi.support.codex_request_builder;

import std;
export import pi.support.responses_compat_resolver;
export import pi.support.responses_request_builder;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.json;
export import pi.types.model;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds request bodies for the ChatGPT Codex backend (openai-codex-responses): the system prompt
 * travels as `instructions`, tools carry `strict: null`, reasoning always includes encrypted
 * content. Port of buildRequestBody in api/openai-codex-responses.ts (grammar tools, additional
 * tools, tool search and service tiers are not ported).
 */
export class CodexRequestBuilder {
public:
    Json build(const Model& model, const TranscriptContext& rawContext, const StreamOptions& options, std::int64_t nowMs) const {
        const ResponsesCompat compat = m_compat.resolve(model);
        const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, compat.supportsMidConvoSystemMessages);
        std::string instructions;
        if (const SystemMessage* system = m_normalizer.initialSystemMessage(context.messages)) {
            instructions = m_normalizer.systemMessageText(*system);
        }
        Json body = Json::object();
        body["model"] = model.id;
        body["store"] = false;
        body["stream"] = true;
        body["instructions"] = instructions.empty() ? "You are a helpful assistant." : instructions;
        body["input"] = m_messages.convertMessages(model, context, compat, {"openai", "openai-codex", "opencode"}, false, nowMs);
        body["text"] = Json{{"verbosity", "low"}};
        body["include"] = Json::array({"reasoning.encrypted_content"});
        if (options.cacheRetention.value_or("short") != "none" && options.sessionId) {
            body["prompt_cache_key"] = clampCacheKey(*options.sessionId);
        }
        body["tool_choice"] = options.toolChoice.value_or("auto");
        body["parallel_tool_calls"] = true;
        if (options.temperature) {
            body["temperature"] = *options.temperature;
        }
        const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
        if (!tools.empty()) {
            const bool strictMode = !model.compat.is_object() || model.compat.value("supportsStrictMode", true);
            body["tools"] = convertTools(tools, strictMode);
        }
        applyReasoning(body, model, options);
        return body;
    }

private:
    Json convertTools(const std::vector<Tool>& tools, bool supportsStrictMode) const {
        Json out = Json::array();
        for (const auto& tool : tools) {
            Json entry = Json{{"type", "function"},
                              {"name", tool.name},
                              {"description", tool.description},
                              {"parameters", tool.parameters}};
            if (supportsStrictMode) {
                entry["strict"] = nullptr;
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    void applyReasoning(Json& body, const Model& model, const StreamOptions& options) const {
        const auto entry = [&](const std::string& name) -> std::optional<Json> {
            if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name)) {
                return model.thinkingLevelMap[name];
            }
            return std::nullopt;
        };
        std::optional<ThinkingLevel> effort;
        if (options.reasoning != ThinkingLevel::Off) {
            const ThinkingLevel clamped = m_levels.clamp(model, options.reasoning);
            if (clamped != ThinkingLevel::Off) {
                effort = clamped;
            }
        }
        if (effort) {
            const std::string name = m_levels.levelName(*effort);
            const auto mapped = entry(name);
            if (!mapped) {
                body["reasoning"] = Json{{"effort", name}, {"summary", "auto"}};
            } else if (!mapped->is_null()) {
                body["reasoning"] = Json{{"effort", *mapped}, {"summary", "auto"}};
            }
        } else if (model.reasoning) {
            const auto off = entry("off");
            if (!off || !off->is_null()) {
                body["reasoning"] = Json{{"effort", off ? *off : Json("none")}};
            }
        }
    }

    std::string clampCacheKey(const std::string& key) const {
        std::size_t points = 0;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if ((static_cast<unsigned char>(key[i]) & 0xC0) != 0x80 && ++points > 64) {
                return key.substr(0, i);
            }
        }
        return key;
    }

    TranscriptNormalizer m_normalizer;
    ResponsesRequestBuilder m_messages;
    ResponsesCompatResolver m_compat;
    ThinkingLevelResolver m_levels;
};
