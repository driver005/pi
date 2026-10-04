export module pi.session.i_settings_manager;

import std;
export import pi.support.settings_view;
export import pi.types.json;
export import pi.types.result;
export import pi.types.settings_error;

/**
 * Global (~/.pi/agent/settings.json) and project (<cwd>/.pi/settings.json) settings. Reads see
 * the merged result (project wins); writes go to one scope and only change the fields touched in
 * this process, so concurrent edits of other fields in the file survive.
 */
export class ISettingsManager {
public:
    virtual ~ISettingsManager() = default;

    /** Effective settings: global and project merged, plus applyOverrides. */
    virtual Json settings() const = 0;
    virtual Json globalSettings() const = 0;
    virtual Json projectSettings() const = 0;
    virtual SettingsView view() const = 0;

    virtual bool projectTrusted() const = 0;
    /** Untrusted projects contribute no settings and cannot be written. */
    virtual Result<void> setProjectTrusted(bool trusted) = 0;

    /** Re-reads both files; read errors are recorded (see drainErrors) and keep the last good values. */
    virtual void reload() = 0;
    /** Session-only overrides on top of the merged settings (CLI flags). */
    virtual void applyOverrides(const Json& overrides) = 0;

    virtual Result<void> setGlobal(const std::string& field, const Json& value) = 0;
    /** Sets one key inside an object field, leaving its other keys as they are in the file. */
    virtual Result<void> setGlobalNested(const std::string& field, const std::string& key, const Json& value) = 0;
    virtual Result<void> removeGlobal(const std::string& field) = 0;
    virtual Result<void> setProject(const std::string& field, const Json& value) = 0;
    virtual Result<void> removeProject(const std::string& field) = 0;

    virtual std::vector<SettingsError> drainErrors() = 0;
    /** Stable UUID of this installation, created on first use (global setting). */
    virtual Result<std::string> getOrCreateDeviceId() = 0;
};
