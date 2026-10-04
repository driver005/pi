module;

#include <cstdint>

export module pi.support.bug_report_builder;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_environment;
export import pi.platform.i_id_generator;
export import pi.platform.i_system_info;
export import pi.provider.i_model_runtime;
export import pi.types.bug_report_bundle;
export import pi.types.bug_report_file;
export import pi.types.bug_report_input;
export import pi.types.crash_record;
export import pi.types.session_entry;
import pi.support.bug_report_redactor;
import pi.support.iso_timestamp;
import pi.support.pi_version;

/**
 * Assembles a bug report: environment and configuration metadata (no secrets), the failed assistant turns of the session
 * without any conversation content, and the files of the bundle. API keys and tokens never appear; header and environment
 * variable names do, their values do not. Port of the collection half of core/bug-report.ts.
 */
export class BugReportBuilder {
public:
    BugReportBuilder(const IEnvironment& environment, const ISystemInfo& system, const IClock& clock, IIdGenerator& ids)
        : m_environment(environment),
          m_system(system),
          m_clock(clock),
          m_ids(ids) {}

    /** The custom session entry type that records a filed report. */
    std::string customEntryType() const {
        return "pi.bug-report";
    }

    std::string archiveFileName(const std::string& id) const {
        return "pi-bug-report-" + id + ".zip";
    }

    /** The provider of the session's model as a report section; null when the provider is unknown. */
    Json describeProvider(const IModelRuntime& models, const std::string& providerId, const std::optional<std::string>& baseUrl) const {
        const std::vector<std::string> ids = models.providerIds();
        if (std::ranges::find(ids, providerId) == ids.end()) {
            return Json(nullptr);
        }
        IModelRuntime& mutableModels = const_cast<IModelRuntime&>(models);
        const AuthStatus status = mutableModels.authStatus(providerId);
        return Json::object({{"id", providerId},
                             {"name", models.providerName(providerId)},
                             {"baseUrl", baseUrl ? Json(m_redactor.redactUrl(*baseUrl)) : Json(nullptr)},
                             {"authStatus", Json::object({{"configured", status.configured}, {"source", status.source.empty() ? Json(nullptr) : Json(status.source)}, {"label", status.label.empty() ? Json(nullptr) : Json(status.label)}})}});
    }

    Json metadata(const BugReportInput& input) const {
        const std::string hint = trim(input.hint.value_or(""));
        Json session = Json::object({{"id", input.sessionId}, {"included", input.includeSession}, {"summaryIncluded", input.includeSummary}, {"messageCount", input.messageCount}});
        if (input.includeSession) {
            session["cwd"] = input.cwd;
        }
        Json plugins = Json::array();
        for (const std::string& path : input.plugins) {
            plugins.push_back(Json::object({{"path", path}, {"source", m_redactor.redactUrl(path)}, {"scope", nullptr}, {"origin", "plugin"}, {"hidden", false}}));
        }
        Json errors = Json::array();
        for (const auto& [path, error] : input.pluginErrors) {
            errors.push_back(Json::object({{"path", path}, {"error", error}}));
        }
        return Json::object({{"schemaVersion", 1},
                             {"id", input.id ? *input.id : m_ids.next()},
                             {"createdAt", m_time.format(m_clock.nowMs())},
                             {"hint", hint.empty() ? Json(nullptr) : Json(hint)},
                             {"environment", environment()},
                             {"session", std::move(session)},
                             {"model", input.model ? describeModel(*input.model) : Json(nullptr)},
                             {"provider", input.provider},
                             {"thinkingLevel", input.thinkingLevel},
                             {"extensions", std::move(plugins)},
                             {"extensionErrors", std::move(errors)},
                             {"settings", Json::object({{"global", redactSettings(input.globalSettings)}, {"project", redactSettings(input.projectSettings)}})}});
    }

    /** Failed assistant turns of the branch (errors, aborts, provider diagnostics) and recent crashes, without conversation content. */
    Json diagnostics(const std::string& sessionId, const std::vector<SessionEntry>& entries, const std::vector<CrashRecord>& crashes) const {
        Json assistant = Json::array();
        std::int64_t assistantCount = 0;
        for (const SessionEntry& entry : entries) {
            if (entry.type != "message" || !entry.body.contains("message") || entry.body["message"].value("role", std::string()) != "assistant") {
                continue;
            }
            ++assistantCount;
            const Json& message = entry.body["message"];
            const Json diagnostics = message.contains("diagnostics") && message["diagnostics"].is_array() ? message["diagnostics"] : Json::array();
            const std::string stop = message.value("stopReason", std::string());
            if (diagnostics.empty() && stop != "error" && stop != "aborted" && !message.contains("errorMessage")) {
                continue;
            }
            Json item = Json::object({{"entryId", entry.id}, {"timestamp", entry.timestamp}, {"provider", message.value("provider", std::string())}, {"model", message.value("model", std::string())}, {"api", message.value("api", std::string())}, {"stopReason", stop}});
            if (message.contains("rawStopReason")) {
                item["rawStopReason"] = message["rawStopReason"];
            }
            if (message.contains("errorMessage")) {
                item["errorMessage"] = message["errorMessage"];
            }
            item["diagnostics"] = diagnostics;
            assistant.push_back(std::move(item));
        }
        Json crashList = Json::array();
        for (const CrashRecord& crash : crashes) {
            crashList.push_back(Json::object({{"timestamp", crash.timestamp}, {"version", crash.version}, {"kind", crash.kind}, {"message", crash.message}, {"stack", crash.stack ? Json(*crash.stack) : Json(nullptr)}, {"sessionFile", crash.sessionFile ? Json(*crash.sessionFile) : Json(nullptr)}, {"cwd", crash.cwd}}));
        }
        return Json::object({{"schemaVersion", 1}, {"sessionId", sessionId}, {"entryCount", static_cast<std::int64_t>(entries.size())}, {"assistantMessageCount", assistantCount}, {"assistant", std::move(assistant)}, {"crashes", std::move(crashList)}});
    }

    /** The files shared by the upload form and the zip export. */
    std::vector<BugReportFile> files(const BugReportBundle& bundle) const {
        std::vector<BugReportFile> out;
        out.push_back({"report.json", "application/json", bundle.metadata.dump(2) + "\n"});
        out.push_back({"diagnostics.json", "application/json", bundle.diagnostics.dump(2) + "\n"});
        if (bundle.sessionJsonl) {
            out.push_back({"session.jsonl", "application/x-ndjson", *bundle.sessionJsonl});
        }
        if (bundle.summary) {
            out.push_back({"summary.md", "text/markdown", bundle.summary->ends_with("\n") ? *bundle.summary : *bundle.summary + "\n"});
        }
        return out;
    }

private:
    Json environment() const {
        const auto value = [this](const std::string& name) -> Json {
            const auto found = m_environment.get(name);
            return found && !found->empty() ? Json(*found) : Json(nullptr);
        };
        std::string shell;
        if (const auto path = m_environment.get("SHELL"); path && !path->empty()) {
            shell = path->substr(path->find_last_of("/\\") == std::string::npos ? 0 : path->find_last_of("/\\") + 1);
        }
        const bool ssh = m_environment.get("SSH_CONNECTION") || m_environment.get("SSH_CLIENT") || m_environment.get("SSH_TTY");
        Json names = Json::array();
        for (const auto& [name, unused] : m_environment.all()) {
            if (name.starts_with("PI_")) {
                names.push_back(name);
            }
        }
        const std::string version = PiVersion().value();
        return Json::object({{"version", version},
                             {"userAgent", "pi/" + version + " (" + m_system.platform() + "; cpp; " + m_system.arch() + ")"},
                             {"runtime", "cpp"},
                             {"platform", m_system.platform()},
                             {"arch", m_system.arch()},
                             {"osRelease", m_system.osRelease()},
                             {"osVersion", m_system.osVersion()},
                             {"shell", shell.empty() ? Json(nullptr) : Json(shell)},
                             {"terminal", Json::object({{"term", value("TERM")}, {"program", value("TERM_PROGRAM")}, {"programVersion", value("TERM_PROGRAM_VERSION")}, {"colorterm", value("COLORTERM")}, {"tmux", m_environment.get("TMUX").has_value()}, {"ssh", ssh}, {"ci", m_environment.get("CI").has_value()}})},
                             // Names help diagnose configuration; values never leave the machine.
                             {"piEnvironmentVariables", std::move(names)}});
    }

    Json describeModel(const Model& model) const {
        Json headerNames = Json::array();
        for (const auto& [name, value] : model.headers) {
            headerNames.push_back(name);
        }
        return Json::object({{"provider", model.provider},
                             {"id", model.id},
                             {"name", model.name},
                             {"api", model.api},
                             {"baseUrl", m_redactor.redactUrl(model.baseUrl)},
                             {"reasoning", model.reasoning},
                             {"input", model.input},
                             {"contextWindow", model.contextWindow},
                             {"maxTokens", model.maxTokens},
                             {"samplingParams", model.samplingParams.is_null() ? Json(nullptr) : m_redactor.redactJson(model.samplingParams)},
                             {"compat", model.compat.is_null() ? Json(nullptr) : m_redactor.redactJson(model.compat)},
                             {"thinkingLevelMap", model.thinkingLevelMap.is_null() ? Json(nullptr) : model.thinkingLevelMap},
                             {"headerNames", std::move(headerNames)}});
    }

    Json redactSettings(const Json& settings) const {
        Json rest = settings.is_object() ? settings : Json::object();
        rest.erase("trackingId");
        rest.erase("deviceId");
        return m_redactor.redactJson(rest);
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    const IEnvironment& m_environment;
    const ISystemInfo& m_system;
    const IClock& m_clock;
    IIdGenerator& m_ids;
    BugReportRedactor m_redactor;
    IsoTimestamp m_time;
};
