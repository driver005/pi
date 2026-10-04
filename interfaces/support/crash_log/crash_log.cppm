module;

#include <cstdint>

export module pi.support.crash_log;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.types.crash_record;
import pi.support.iso_timestamp;
import pi.support.pi_version;
import pi.types.json;

/**
 * The last few crashes of pi, kept in `<agent dir>/crashes.json` (at most five, newest last) so the next start or a bug report
 * can mention them. Recording is best effort: it is called while the process is already failing and reports nothing back.
 * Port of core/crash-log.ts (the stack-to-extension matching of JavaScript stack traces has no counterpart).
 */
export class CrashLog {
public:
    CrashLog(IFileSystem& files, const IClock& clock)
        : m_files(files),
          m_clock(clock) {}

    std::string path(const std::string& agentDir) const {
        return agentDir + "/crashes.json";
    }

    /** The valid records of the file, oldest first; a missing or unreadable file is an empty log. */
    std::vector<CrashRecord> read(const std::string& path) const {
        std::vector<CrashRecord> out;
        const auto text = m_files.readFile(path);
        if (!text) {
            return out;
        }
        const Json json = Json::parse(*text, nullptr, false);
        if (!json.is_array()) {
            return out;
        }
        for (const Json& item : json) {
            if (item.is_object() && item.contains("timestamp") && item["timestamp"].is_string() && item.contains("message") && item["message"].is_string()) {
                out.push_back(fromJson(item));
            }
        }
        return out;
    }

    /** Appends a crash, keeping the newest five; nullopt when the file could not be written. */
    std::optional<CrashRecord> record(const std::string& path, const std::string& kind, const std::string& message, const std::optional<std::string>& stack, const std::optional<std::string>& sessionFile, const std::string& cwd) const {
        CrashRecord crash;
        crash.timestamp = m_time.format(m_clock.nowMs());
        crash.version = PiVersion().value();
        crash.kind = kind;
        crash.message = message;
        crash.stack = stack;
        crash.sessionFile = sessionFile;
        crash.cwd = cwd;
        std::vector<CrashRecord> records = read(path);
        records.push_back(crash);
        if (records.size() > kMaxRecords) {
            records.erase(records.begin(), records.end() - kMaxRecords);
        }
        return write(records, path) ? std::optional<CrashRecord>(crash) : std::nullopt;
    }

    /** The newest crash from the last seven days that was not announced yet; every record is marked announced. */
    std::optional<CrashRecord> takeUnnotified(const std::string& path) const {
        std::vector<CrashRecord> records = read(path);
        const std::int64_t now = m_clock.nowMs();
        std::optional<CrashRecord> found;
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            const auto time = m_time.parse(it->timestamp);
            if (!it->notified && time && now - *time <= kMaxAgeMs) {
                found = *it;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        for (CrashRecord& record : records) {
            record.notified = true;
        }
        write(records, path);
        return found;
    }

    void clear(const std::string& path) const {
        m_files.removeFile(path);
    }

private:
    static constexpr std::size_t kMaxRecords = 5;
    static constexpr std::int64_t kMaxAgeMs = 7LL * 24 * 60 * 60 * 1000;

    bool write(const std::vector<CrashRecord>& records, const std::string& path) const {
        Json array = Json::array();
        for (const CrashRecord& record : records) {
            array.push_back(toJson(record));
        }
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            m_files.createDirectories(path.substr(0, slash));
        }
        return m_files.writeFile(path, array.dump(2) + "\n").has_value();
    }

    Json toJson(const CrashRecord& record) const {
        Json out = Json::object({{"timestamp", record.timestamp}, {"version", record.version}, {"kind", record.kind}, {"message", record.message}, {"stack", record.stack ? Json(*record.stack) : Json(nullptr)}, {"sessionFile", record.sessionFile ? Json(*record.sessionFile) : Json(nullptr)}, {"cwd", record.cwd}});
        if (record.notified) {
            out["notified"] = true;
        }
        return out;
    }

    CrashRecord fromJson(const Json& json) const {
        CrashRecord record;
        record.timestamp = json["timestamp"].get<std::string>();
        record.message = json["message"].get<std::string>();
        record.version = json.value("version", std::string());
        record.kind = json.value("kind", std::string());
        record.cwd = json.value("cwd", std::string());
        if (json.contains("stack") && json["stack"].is_string()) {
            record.stack = json["stack"].get<std::string>();
        }
        if (json.contains("sessionFile") && json["sessionFile"].is_string()) {
            record.sessionFile = json["sessionFile"].get<std::string>();
        }
        record.notified = json.contains("notified") && json["notified"] == true;
        return record;
    }

    IFileSystem& m_files;
    const IClock& m_clock;
    IsoTimestamp m_time;
};
