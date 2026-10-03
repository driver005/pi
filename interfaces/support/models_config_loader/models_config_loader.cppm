export module pi.support.models_config_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.json_comment_stripper;
export import pi.types.json;
export import pi.types.result;

/**
 * Reads models.json: {"providers": {"<id>": {baseUrl, apiKey, headers, models, modelOverrides,
 * ...}}}. Comments and trailing commas are allowed. A missing file is an empty config. Only the
 * structure is validated here; field-level checks happen when the config is applied.
 */
export class ModelsConfigLoader {
public:
    explicit ModelsConfigLoader(IFileSystem& files);

    /** The "providers" object ({} when absent), or an Error describing the first problem. */
    Result<Json> load(const std::string& path);

private:
    IFileSystem& m_files;
    JsonCommentStripper m_stripper;
};

ModelsConfigLoader::ModelsConfigLoader(IFileSystem& files) : m_files(files) {}

Result<Json> ModelsConfigLoader::load(const std::string& path) {
    auto content = m_files.readFile(path);
    if (!content) {
        if (content.error().code == "ENOENT") {
            return Json::object();
        }
        return std::unexpected(Error{"models_config", "Failed to load models.json: " +
                                                         content.error().message + "\n\nFile: " + path});
    }
    std::string text = *content;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
        text.erase(0, 3);
    }
    const Json parsed = Json::parse(m_stripper.strip(text), nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(
            Error{"models_config", "Failed to parse models.json: invalid JSON\n\nFile: " + path});
    }
    if (!parsed.is_object() || !parsed.contains("providers") || !parsed["providers"].is_object()) {
        return std::unexpected(Error{
            "models_config",
            "Invalid models.json schema:\n  - providers: expected an object\n\nFile: " + path});
    }
    for (const auto& entry : parsed["providers"].items()) {
        const std::string& id = entry.key();
        const Json& provider = entry.value();
        if (!provider.is_object()) {
            return std::unexpected(Error{
                "models_config", "Invalid models.json schema:\n  - providers." + id +
                                     ": expected an object\n\nFile: " + path});
        }
    }
    return parsed["providers"];
}
