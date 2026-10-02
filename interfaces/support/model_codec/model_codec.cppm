module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.model_codec;

import std;
export import pi.support.json_reader;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;

/** Model <-> JSON in the shape of the TypeScript catalog (models.json, generated catalogs). */
export class ModelCodec {
public:
    Json toJson(const Model& model) const;
    Json costToJson(const ModelCost& cost) const;

    /** `type` other than "chat" (image and classifier entries) is an error. */
    Result<Model> fromJson(const Json& json) const;
    ModelCost costFromJson(const Json& json) const;

private:
    std::vector<std::string> inputFromJson(const Json& json) const;
    std::map<std::string, std::string> headersFromJson(const Json& json) const;
};

Json ModelCodec::costToJson(const ModelCost& cost) const {
    Json out = Json::object();
    out["input"] = cost.input;
    out["output"] = cost.output;
    out["cacheRead"] = cost.cacheRead;
    out["cacheWrite"] = cost.cacheWrite;
    if (!cost.tiers.empty()) {
        Json tiers = Json::array();
        for (const auto& tier : cost.tiers) {
            Json entry = Json::object();
            entry["inputTokensAbove"] = tier.inputTokensAbove;
            entry["input"] = tier.input;
            entry["output"] = tier.output;
            entry["cacheRead"] = tier.cacheRead;
            entry["cacheWrite"] = tier.cacheWrite;
            tiers.push_back(std::move(entry));
        }
        out["tiers"] = std::move(tiers);
    }
    return out;
}

Json ModelCodec::toJson(const Model& model) const {
    Json out = Json::object();
    out["id"] = model.id;
    out["name"] = model.name;
    out["api"] = model.api;
    out["provider"] = model.provider;
    out["baseUrl"] = model.baseUrl;
    out["input"] = model.input;
    if (!model.inputLimits.is_null()) {
        out["inputLimits"] = model.inputLimits;
    }
    out["cost"] = costToJson(model.cost);
    if (!model.headers.empty()) {
        Json headers = Json::object();
        for (const auto& [name, value] : model.headers) {
            headers[name] = value;
        }
        out["headers"] = std::move(headers);
    }
    out["reasoning"] = model.reasoning;
    if (!model.thinkingLevelMap.is_null()) {
        out["thinkingLevelMap"] = model.thinkingLevelMap;
    }
    if (!model.promptCache.is_null()) {
        out["promptCache"] = model.promptCache;
    }
    out["contextWindow"] = model.contextWindow;
    out["maxTokens"] = model.maxTokens;
    if (!model.samplingParams.is_null()) {
        out["samplingParams"] = model.samplingParams;
    }
    if (!model.compat.is_null()) {
        out["compat"] = model.compat;
    }
    return out;
}

ModelCost ModelCodec::costFromJson(const Json& json) const {
    ModelCost cost;
    JsonReader reader(json);
    cost.input = reader.optNumber("input").value_or(0);
    cost.output = reader.optNumber("output").value_or(0);
    cost.cacheRead = reader.optNumber("cacheRead").value_or(0);
    cost.cacheWrite = reader.optNumber("cacheWrite").value_or(0);
    if (json.is_object() && json.contains("tiers") && json["tiers"].is_array()) {
        for (const auto& item : json["tiers"]) {
            JsonReader tier(item);
            ModelCostTier entry;
            entry.inputTokensAbove = tier.optInt("inputTokensAbove").value_or(0);
            entry.input = tier.optNumber("input").value_or(0);
            entry.output = tier.optNumber("output").value_or(0);
            entry.cacheRead = tier.optNumber("cacheRead").value_or(0);
            entry.cacheWrite = tier.optNumber("cacheWrite").value_or(0);
            cost.tiers.push_back(entry);
        }
    }
    return cost;
}

std::vector<std::string> ModelCodec::inputFromJson(const Json& json) const {
    std::vector<std::string> input;
    if (json.is_array()) {
        for (const auto& item : json) {
            if (item.is_string()) {
                input.push_back(item.get<std::string>());
            }
        }
    }
    if (input.empty()) {
        input.push_back("text");
    }
    return input;
}

std::map<std::string, std::string> ModelCodec::headersFromJson(const Json& json) const {
    std::map<std::string, std::string> headers;
    if (json.is_object()) {
        for (const auto& [name, value] : json.items()) {
            if (value.is_string()) {
                headers[name] = value.get<std::string>();
            }
        }
    }
    return headers;
}

Result<Model> ModelCodec::fromJson(const Json& json) const {
    JsonReader reader(json);
    if (!reader.isObject()) {
        return std::unexpected(Error{"invalid_model", "model must be an object"});
    }
    if (const auto type = reader.optString("type"); type && *type != "chat") {
        return std::unexpected(Error{"unsupported_model_type", "unsupported model type: " + *type});
    }
    auto id = reader.requireString("id");
    if (!id) {
        return std::unexpected(id.error());
    }
    Model model;
    model.id = *id;
    model.name = reader.optString("name").value_or(model.id);
    model.api = reader.optString("api").value_or("");
    model.provider = reader.optString("provider").value_or("");
    model.baseUrl = reader.optString("baseUrl").value_or("");
    model.input = inputFromJson(reader.get("input"));
    model.inputLimits = reader.get("inputLimits");
    model.cost = costFromJson(reader.get("cost"));
    model.headers = headersFromJson(reader.get("headers"));
    model.reasoning = reader.optBool("reasoning").value_or(false);
    model.thinkingLevelMap = reader.get("thinkingLevelMap");
    model.promptCache = reader.get("promptCache");
    model.contextWindow = reader.optInt("contextWindow").value_or(0);
    model.maxTokens = reader.optInt("maxTokens").value_or(0);
    model.samplingParams = reader.get("samplingParams");
    model.compat = reader.get("compat");
    return model;
}
