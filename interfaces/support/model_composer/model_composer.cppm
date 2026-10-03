module;

#include <cstdint>

export module pi.support.model_composer;

import std;
export import pi.support.model_codec;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;

/**
 * Applies one provider's models.json section to the built-in models: provider-level baseUrl and
 * compat, custom model definitions (added or replacing by id) and modelOverrides. Port of
 * applyModelsJson / modelFromJson / applyModelOverride in core/provider-composer.ts.
 */
export class ModelComposer {
public:
    /** `config` is the provider's object from models.json; null leaves the base models alone. */
    Result<std::vector<Model>> compose(const std::string& providerId, const std::vector<Model>& base,
                                       const Json& config) const;

    Model applyOverride(const Model& model, const Json& override) const;

    /** {...base, ...override} with one level of merging for the routing/template objects. */
    Json mergeCompat(const Json& base, const Json& override) const;

private:
    Result<Model> modelFromDefinition(const std::string& providerId, const Json& definition,
                                      const Json& config, const Model* defaults) const;
    const Model* findDefaults(const std::vector<Model>& models, const std::string& id,
                              const std::string& api) const;
    Json mergeObjects(const Json& base, const Json& override) const;
    Json mergeInputLimits(const Json& base, const Json& override) const;
    std::string stringField(const Json& object, const std::string& key) const;
    bool hasUsefulKeys(const Json& config) const;

    ModelCodec m_codec;
};

std::string ModelComposer::stringField(const Json& object, const std::string& key) const {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return "";
}

Json ModelComposer::mergeObjects(const Json& base, const Json& override) const {
    Json merged = base.is_object() ? base : Json::object();
    if (override.is_object()) {
        for (const auto& entry : override.items()) {
            const std::string& key = entry.key();
            const Json& value = entry.value();
            merged[key] = value;
        }
    }
    return merged;
}

Json ModelComposer::mergeCompat(const Json& base, const Json& override) const {
    if (!override.is_object()) {
        return base;
    }
    Json merged = mergeObjects(base, override);
    for (const char* key : {"openRouterRouting", "vercelGatewayRouting", "chatTemplateKwargs",
                            "chatTemplateArgs"}) {
        const bool baseHas = base.is_object() && base.contains(key) && base[key].is_object();
        const bool overrideHas = override.contains(key) && override[key].is_object();
        if (baseHas || overrideHas) {
            merged[key] = mergeObjects(baseHas ? base[key] : Json::object(),
                                       overrideHas ? override[key] : Json::object());
        }
    }
    return merged;
}

Json ModelComposer::mergeInputLimits(const Json& base, const Json& override) const {
    if (!override.is_object()) {
        return base;
    }
    Json merged = mergeObjects(base, override);
    if (override.contains("images") && override["images"].is_object()) {
        const Json baseImages = base.is_object() && base.contains("images") ? base["images"] : Json::object();
        Json images = mergeObjects(baseImages, override["images"]);
        if (override["images"].contains("resize") && override["images"]["resize"].is_object()) {
            const Json baseResize =
                baseImages.is_object() && baseImages.contains("resize") ? baseImages["resize"] : Json::object();
            images["resize"] = mergeObjects(baseResize, override["images"]["resize"]);
        }
        merged["images"] = std::move(images);
    }
    return merged;
}

Model ModelComposer::applyOverride(const Model& model, const Json& override) const {
    Model out = model;
    if (!override.is_object()) {
        return out;
    }
    if (const auto name = stringField(override, "name"); !name.empty()) {
        out.name = name;
    }
    if (override.contains("reasoning") && override["reasoning"].is_boolean()) {
        out.reasoning = override["reasoning"].get<bool>();
    }
    if (override.contains("thinkingLevelMap") && override["thinkingLevelMap"].is_object()) {
        out.thinkingLevelMap = mergeObjects(model.thinkingLevelMap, override["thinkingLevelMap"]);
    }
    if (override.contains("input") && override["input"].is_array()) {
        out.input = override["input"].get<std::vector<std::string>>();
    }
    if (override.contains("inputLimits")) {
        out.inputLimits = mergeInputLimits(model.inputLimits, override["inputLimits"]);
    }
    if (override.contains("cost") && override["cost"].is_object()) {
        const Json& cost = override["cost"];
        const ModelCost parsed = m_codec.costFromJson(cost);
        out.cost.input = cost.contains("input") ? parsed.input : model.cost.input;
        out.cost.output = cost.contains("output") ? parsed.output : model.cost.output;
        out.cost.cacheRead = cost.contains("cacheRead") ? parsed.cacheRead : model.cost.cacheRead;
        out.cost.cacheWrite = cost.contains("cacheWrite") ? parsed.cacheWrite : model.cost.cacheWrite;
        out.cost.tiers = cost.contains("tiers") ? parsed.tiers : model.cost.tiers;
    }
    if (override.contains("promptCache") && override["promptCache"].is_object()) {
        out.promptCache = mergeObjects(model.promptCache, override["promptCache"]);
    }
    if (override.contains("contextWindow") && override["contextWindow"].is_number()) {
        out.contextWindow = override["contextWindow"].get<std::int64_t>();
    }
    if (override.contains("maxTokens") && override["maxTokens"].is_number()) {
        out.maxTokens = override["maxTokens"].get<std::int64_t>();
    }
    if (override.contains("samplingParams") && override["samplingParams"].is_object()) {
        out.samplingParams = mergeObjects(model.samplingParams, override["samplingParams"]);
    }
    if (override.contains("headers") && override["headers"].is_object()) {
        // Header values stay references (env, commands) until request time; see ModelRuntime.
    }
    out.compat = mergeCompat(model.compat, override.contains("compat") ? override["compat"] : Json());
    return out;
}

const Model* ModelComposer::findDefaults(const std::vector<Model>& models, const std::string& id,
                                         const std::string& api) const {
    for (const auto& model : models) {
        if (model.id == id) {
            return &model;
        }
    }
    if (!api.empty()) {
        for (const auto& model : models) {
            if (model.api == api) {
                return &model;
            }
        }
    }
    for (const auto& model : models) {
        if (model.api == "openai-completions") {
            return &model;
        }
    }
    return models.empty() ? nullptr : &models.front();
}

Result<Model> ModelComposer::modelFromDefinition(const std::string& providerId,
                                                 const Json& definition, const Json& config,
                                                 const Model* defaults) const {
    const std::string id = stringField(definition, "id");
    std::string api = stringField(definition, "api");
    if (api.empty()) {
        api = stringField(config, "api");
    }
    if (api.empty() && defaults != nullptr) {
        api = defaults->api;
    }
    if (api.empty()) {
        return std::unexpected(Error{"models_config", "Provider " + providerId + ", model " + id +
                                                           ": no \"api\" specified. Set at provider or model level."});
    }
    std::string baseUrl = stringField(definition, "baseUrl");
    if (baseUrl.empty()) {
        baseUrl = stringField(config, "baseUrl");
    }
    if (baseUrl.empty() && defaults != nullptr) {
        baseUrl = defaults->baseUrl;
    }
    if (baseUrl.empty()) {
        return std::unexpected(Error{"models_config", "Provider " + providerId +
                                                           ": \"baseUrl\" is required when defining custom models."});
    }
    for (const char* field : {"contextWindow", "maxTokens"}) {
        if (definition.contains(field) && definition[field].is_number() && definition[field].get<double>() <= 0) {
            return std::unexpected(Error{"models_config", "Provider " + providerId + ", model " + id +
                                                               ": invalid " + field});
        }
    }
    Json json = Json::object();
    json["id"] = id;
    json["name"] = definition.contains("name") ? definition["name"] : Json(id);
    json["api"] = api;
    json["provider"] = providerId;
    json["baseUrl"] = baseUrl;
    for (const char* field : {"reasoning", "thinkingLevelMap", "input", "inputLimits", "cost", "promptCache",
                              "samplingParams"}) {
        if (definition.contains(field)) {
            json[field] = definition[field];
        }
    }
    json["contextWindow"] = definition.contains("contextWindow") ? definition["contextWindow"] : Json(128000);
    json["maxTokens"] = definition.contains("maxTokens") ? definition["maxTokens"] : Json(16384);
    auto model = m_codec.fromJson(json);
    if (!model) {
        return std::unexpected(model.error());
    }
    model->compat = mergeCompat(config.is_object() && config.contains("compat") ? config["compat"] : Json(),
                                definition.contains("compat") ? definition["compat"] : Json());
    return model;
}

bool ModelComposer::hasUsefulKeys(const Json& config) const {
    const auto non_empty = [&](const char* key) {
        return config.contains(key) && !config[key].is_null() &&
               !(config[key].is_array() && config[key].empty()) &&
               !(config[key].is_object() && config[key].empty());
    };
    return non_empty("models") || non_empty("baseUrl") || non_empty("headers") || non_empty("compat") ||
           non_empty("modelOverrides") || non_empty("apiKey") || non_empty("oauth") ||
           config.contains("authHeader");
}

Result<std::vector<Model>> ModelComposer::compose(const std::string& providerId,
                                                  const std::vector<Model>& base,
                                                  const Json& config) const {
    if (!config.is_object()) {
        return base;
    }
    const bool oauth = config.contains("oauth") && !config["oauth"].is_null();
    if (oauth && stringField(config, "baseUrl").empty()) {
        return std::unexpected(Error{"models_config", "Provider " + providerId +
                                                           ": \"baseUrl\" is required when \"oauth\" is set."});
    }
    if (!hasUsefulKeys(config)) {
        return std::unexpected(Error{"models_config",
                                     "Provider " + providerId +
                                         ": must specify \"baseUrl\", \"headers\", \"compat\", \"modelOverrides\", or \"models\"."});
    }
    std::vector<Model> models = base;
    const std::string configBase = stringField(config, "baseUrl");
    for (auto& model : models) {
        const bool radius = stringField(config, "oauth") == "radius";
        if (!radius && !configBase.empty()) {
            model.baseUrl = configBase;
        }
        model.compat = mergeCompat(model.compat, config.contains("compat") ? config["compat"] : Json());
    }
    if (config.contains("models") && config["models"].is_array()) {
        for (const auto& definition : config["models"]) {
            const std::string id = stringField(definition, "id");
            const auto existing = std::find_if(models.begin(), models.end(),
                                               [&](const Model& model) { return model.id == id; });
            const Model* defaults = findDefaults(models, id, [&]() {
                const std::string api = stringField(definition, "api");
                return api.empty() ? stringField(config, "api") : api;
            }());
            auto built = modelFromDefinition(providerId, definition, config, defaults);
            if (!built) {
                return std::unexpected(built.error());
            }
            if (existing != models.end()) {
                *existing = *built;
            } else {
                models.push_back(*built);
            }
        }
    }
    if (config.contains("modelOverrides") && config["modelOverrides"].is_object()) {
        for (auto& model : models) {
            if (config["modelOverrides"].contains(model.id)) {
                model = applyOverride(model, config["modelOverrides"][model.id]);
            }
        }
    }
    return models;
}
