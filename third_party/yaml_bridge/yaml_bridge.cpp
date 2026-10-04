#include "third_party/yaml_bridge/yaml_bridge.h"

#include <cstdlib>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace {

using Json = nlohmann::ordered_json;

Json scalarToJson(const YAML::Node& node) {
    const std::string text = node.Scalar();
    if (node.Tag() == "!") {
        return text;
    }
    if (text == "true" || text == "false") {
        return text == "true";
    }
    if (text == "null" || text == "~") {
        return nullptr;
    }
    if (text.empty()) {
        return "";
    }
    char* end = nullptr;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0') {
        return integer;
    }
    const double real = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && *end == '\0') {
        return real;
    }
    return text;
}

Json nodeToJson(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            Json object = Json::object();
            for (const auto& entry : node) {
                object[entry.first.as<std::string>()] = nodeToJson(entry.second);
            }
            return object;
        }
        case YAML::NodeType::Sequence: {
            Json array = Json::array();
            for (const auto& entry : node) {
                array.push_back(nodeToJson(entry));
            }
            return array;
        }
        case YAML::NodeType::Scalar:
            return scalarToJson(node);
        default:
            return nullptr;
    }
}

}  // namespace

bool yamlToJson(const std::string& yaml, std::string* json, std::string* error) {
    try {
        *json = nodeToJson(YAML::Load(yaml)).dump();
        return true;
    } catch (const std::exception& failure) {
        *error = failure.what();
        return false;
    }
}
