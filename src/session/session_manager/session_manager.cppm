module;

#include <cstdint>

export module pi.session.session_manager;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.session.i_session_manager;
export import pi.support.agent_message_codec;
export import pi.support.iso_timestamp;
export import pi.support.message_codec;
export import pi.support.path_resolver;
export import pi.support.session_entry_codec;
export import pi.support.session_migrator;
export import pi.support.session_projector;
export import pi.support.short_id_generator;
export import pi.support.transcript_normalizer;
export import pi.types.session_manager_options;

/**
 * ISessionManager over a JSONL file (or memory). The file is created once the session holds a
 * user or assistant message, then appended entry by entry. Port of SessionManager in
 * packages/coding-agent/src/core/session-manager.ts; files are interchangeable with the TS one.
 */
export class SessionManager : public ISessionManager {
public:
    static constexpr int CurrentVersion = 3;

    SessionManager(SessionManagerOptions options, IFileSystem& files, const IClock& clock,
                   IIdGenerator& ids);

    /** Result of opening: Error when an explicit session file is not a valid session. */
    Result<void> open();

    std::string cwd() const override;
    std::string sessionDir() const override;
    std::string sessionId() const override;
    std::optional<std::string> sessionFile() const override;
    bool isPersisted() const override;
    Result<std::optional<std::string>> newSession(const std::optional<std::string>& id,
                                                  const std::optional<std::string>& parentSession) override;
    Result<void> setSessionFile(const std::string& path) override;

    Result<std::string> appendMessage(const AgentMessage& message) override;
    Result<std::string> appendThinkingLevelChange(const std::string& thinkingLevel) override;
    Result<std::string> appendModelChange(const std::string& provider, const std::string& modelId) override;
    Result<SessionEntry> appendUsage(const std::string& kind, const std::string& provider,
                                     const std::string& model, const Usage& usage,
                                     const std::optional<std::string>& note) override;
    Result<std::string> appendCompaction(const std::string& summary,
                                         const std::optional<std::string>& firstKeptEntryId,
                                         std::int64_t tokensBefore, const Json& details,
                                         std::optional<bool> fromHook,
                                         const std::optional<Usage>& usage) override;
    Result<std::string> appendCustomEntry(const std::string& customType, const Json& data) override;
    Result<std::string> appendSessionInfo(const std::string& name) override;
    Result<std::string> appendCustomMessageEntry(const std::string& customType, const Json& content,
                                                 bool display, const Json& details) override;
    Result<std::string> appendContextEdit(const std::string& targetId,
                                          const std::optional<Json>& replacement) override;
    Result<std::string> appendLabelChange(const std::string& targetId,
                                          const std::optional<std::string>& label) override;

    std::optional<std::string> leafId() const override;
    std::optional<SessionEntry> leafEntry() const override;
    std::optional<SessionEntry> entry(const std::string& id) const override;
    std::vector<SessionEntry> children(const std::string& parentId) const override;
    std::optional<std::string> label(const std::string& id) const override;
    std::vector<SessionEntry> branchPath(const std::optional<std::string>& fromId) const override;
    std::vector<SessionEntry> buildContextEntries() const override;
    SessionProjection buildSessionProjection() const override;
    SessionContext buildSessionContext() const override;
    std::optional<SessionHeader> header() const override;
    std::size_t entryCount() const override;
    std::vector<SessionEntry> entries() const override;
    std::vector<SessionTreeNode> tree() const override;
    std::optional<std::string> sessionName() const override;

    Result<void> branch(const std::string& branchFromId) override;
    void resetLeaf() override;
    Result<std::string> branchWithSummary(const std::optional<std::string>& branchFromId,
                                          const std::string& summary, const Json& details,
                                          std::optional<bool> fromHook,
                                          const std::optional<Usage>& usage) override;
    Result<std::optional<std::string>> createBranchedSession(const std::string& leafId) override;

private:
    using Labels = std::map<std::string, std::string>;

    std::string nowIso() const;
    std::string newEntryId();
    Json baseEntry(const std::string& type);
    Result<std::string> appendBody(Json body);
    Result<void> persist(const Json& body);
    Result<void> rewriteFile();
    Result<void> writeNewFile();
    bool hasConversation() const;
    Result<std::vector<Json>> loadFile(const std::string& path);
    Result<void> loadEntries(std::vector<Json> entries);
    void buildIndex();
    Json makeHeader(const std::string& id, const std::string& timestamp,
                    const std::optional<std::string>& parent) const;
    std::string fileFor(const std::string& timestamp, const std::string& id) const;
    Result<void> assertValidId(const std::string& id) const;
    std::vector<SessionEntry> sessionEntries() const;
    Result<void> openFile(const std::string& path, std::vector<Json> preloaded);
    std::string resolve(const std::string& path) const;
    Json usageJson(const Usage& usage) const;

    SessionManagerOptions m_options;
    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
    PathResolver m_paths;
    SessionEntryCodec m_entries;
    SessionMigrator m_migrator;
    SessionProjector m_projector;
    IsoTimestamp m_iso;
    AgentMessageCodec m_messages;
    MessageCodec m_codec;
    TranscriptNormalizer m_normalizer;
    ShortIdGenerator m_shortIds;

    std::string m_cwd;
    std::string m_sessionDir;
    std::string m_sessionId;
    std::optional<std::string> m_sessionFile;
    bool m_persist = true;
    bool m_flushed = false;
    std::vector<Json> m_fileEntries;
    std::map<std::string, SessionEntry> m_byId;
    Labels m_labels;
    Labels m_labelTimestamps;
    std::optional<std::string> m_leafId;
};

SessionManager::SessionManager(SessionManagerOptions options, IFileSystem& files, const IClock& clock,
                               IIdGenerator& ids)
    : m_options(std::move(options)),
      m_files(files),
      m_clock(clock),
      m_ids(ids),
      m_paths(files.homeDirectory()),
      m_persist(m_options.persist) {
    m_cwd = m_options.cwd.empty() ? "" : resolve(m_options.cwd);
    m_sessionDir = m_options.sessionDir.empty() ? "" : resolve(m_options.sessionDir);
}

std::string SessionManager::resolve(const std::string& path) const {
    return m_paths.resolveToCwd(path, "/");
}

Result<void> SessionManager::open() {
    if (m_persist && !m_sessionDir.empty() && !m_files.exists(m_sessionDir)) {
        if (auto created = m_files.createDirectories(m_sessionDir); !created) {
            return created;
        }
    }
    if (m_options.sessionFile) {
        return openFile(resolve(*m_options.sessionFile), std::move(m_options.preloadedEntries));
    }
    if (!m_options.preloadedEntries.empty()) {
        return loadEntries(std::move(m_options.preloadedEntries));
    }
    auto started = newSession(m_options.id, m_options.parentSession);
    if (!started) {
        return std::unexpected(started.error());
    }
    return {};
}

std::string SessionManager::nowIso() const {
    return m_iso.format(m_clock.nowMs());
}

Result<void> SessionManager::assertValidId(const std::string& id) const {
    const auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    bool valid = !id.empty() && alnum(id.front()) && alnum(id.back());
    for (const char c : id) {
        valid = valid && (alnum(c) || c == '.' || c == '_' || c == '-');
    }
    if (!valid) {
        return std::unexpected(Error{"invalid_session_id",
                                     "Session id must be non-empty, contain only alphanumeric characters, '-', '_', "
                                     "and '.', and start and end with an alphanumeric character"});
    }
    return {};
}

std::string SessionManager::fileFor(const std::string& timestamp, const std::string& id) const {
    std::string stamp = timestamp;
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    std::replace(stamp.begin(), stamp.end(), '.', '-');
    return m_sessionDir + "/" + stamp + "_" + id + ".jsonl";
}

Json SessionManager::makeHeader(const std::string& id, const std::string& timestamp,
                                const std::optional<std::string>& parent) const {
    Json header = Json::object();
    header["type"] = "session";
    header["version"] = CurrentVersion;
    header["id"] = id;
    header["timestamp"] = timestamp;
    header["cwd"] = m_cwd;
    if (parent) {
        header["parentSession"] = *parent;
    }
    return header;
}

Result<std::optional<std::string>> SessionManager::newSession(
    const std::optional<std::string>& id, const std::optional<std::string>& parentSession) {
    if (id) {
        if (auto valid = assertValidId(*id); !valid) {
            return std::unexpected(valid.error());
        }
    }
    m_sessionId = id ? *id : m_ids.next();
    const std::string timestamp = nowIso();
    m_fileEntries = {makeHeader(m_sessionId, timestamp, parentSession)};
    m_byId.clear();
    m_labels.clear();
    m_labelTimestamps.clear();
    m_leafId.reset();
    m_flushed = false;
    if (m_persist) {
        m_sessionFile = fileFor(timestamp, m_sessionId);
    }
    return m_sessionFile;
}

Result<std::vector<Json>> SessionManager::loadFile(const std::string& path) {
    auto content = m_files.readFile(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    std::vector<Json> entries = m_entries.parseLines(*content);
    if (entries.empty()) {
        return entries;
    }
    if (!m_entries.isHeader(entries.front()) || !entries.front().contains("id") ||
        !entries.front()["id"].is_string()) {
        return std::vector<Json>{};
    }
    if (!content->empty() && content->back() != '\n') {
        // Repair a missing trailing newline so the next append starts its own line.
        if (auto repaired = m_files.appendFile(path, "\n"); !repaired) {
            return std::unexpected(repaired.error());
        }
    }
    return entries;
}

Result<void> SessionManager::openFile(const std::string& path, std::vector<Json> preloaded) {
    m_sessionFile = path;
    if (!m_files.exists(path)) {
        auto started = newSession(m_options.id, m_options.parentSession);
        if (!started) {
            return std::unexpected(started.error());
        }
        m_sessionFile = path;
        return {};
    }
    std::vector<Json> entries;
    if (!preloaded.empty()) {
        entries = std::move(preloaded);
    } else {
        auto loaded = loadFile(path);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        entries = std::move(*loaded);
    }
    if (entries.empty()) {
        const auto info = m_files.stat(path);
        if (info && info->size > 0) {
            return std::unexpected(Error{"invalid_session", "Session file is not a valid pi session: " + path});
        }
        auto started = newSession(std::nullopt, std::nullopt);
        if (!started) {
            return std::unexpected(started.error());
        }
        m_sessionFile = path;
        if (auto rewritten = rewriteFile(); !rewritten) {
            return rewritten;
        }
        m_flushed = true;
        return {};
    }
    if (auto loaded = loadEntries(std::move(entries)); !loaded) {
        return loaded;
    }
    m_flushed = true;
    return {};
}

Result<void> SessionManager::setSessionFile(const std::string& path) {
    return openFile(resolve(path), {});
}

Result<void> SessionManager::loadEntries(std::vector<Json> entries) {
    const bool hasHeader = std::any_of(entries.begin(), entries.end(),
                                       [&](const Json& entry) { return m_entries.isHeader(entry); });
    if (hasHeader) {
        m_fileEntries = std::move(entries);
        for (const auto& entry : m_fileEntries) {
            if (m_entries.isHeader(entry)) {
                m_sessionId = entry.value("id", "");
                // Resuming without an explicit cwd continues where the session was recorded.
                if (m_cwd.empty()) {
                    m_cwd = entry.value("cwd", "");
                }
                break;
            }
        }
        if (m_migrator.migrate(m_fileEntries)) {
            if (auto rewritten = rewriteFile(); !rewritten) {
                return rewritten;
            }
        }
    } else {
        auto started = newSession(m_options.id, m_options.parentSession);
        if (!started) {
            return std::unexpected(started.error());
        }
        for (auto& entry : entries) {
            m_fileEntries.push_back(std::move(entry));
        }
    }
    buildIndex();
    return {};
}

void SessionManager::buildIndex() {
    m_byId.clear();
    m_labels.clear();
    m_labelTimestamps.clear();
    m_leafId.reset();
    for (const auto& json : m_fileEntries) {
        if (m_entries.isHeader(json)) {
            continue;
        }
        SessionEntry entry = m_entries.entryFromJson(json);
        m_leafId = entry.id;
        if (entry.type == "label") {
            const std::string target = entry.body.value("targetId", "");
            if (entry.body.contains("label") && entry.body["label"].is_string() &&
                !entry.body["label"].get<std::string>().empty()) {
                m_labels[target] = entry.body["label"].get<std::string>();
                m_labelTimestamps[target] = entry.timestamp;
            } else {
                m_labels.erase(target);
                m_labelTimestamps.erase(target);
            }
        }
        m_byId[entry.id] = std::move(entry);
    }
}

Result<void> SessionManager::rewriteFile() {
    if (!m_persist || !m_sessionFile) {
        return {};
    }
    std::string text;
    for (const auto& entry : m_fileEntries) {
        text += m_entries.line(entry);
    }
    return m_files.writeFile(*m_sessionFile, text);
}

bool SessionManager::hasConversation() const {
    for (const auto& json : m_fileEntries) {
        if (json.value("type", "") == "message" && json.contains("message") && json["message"].is_object()) {
            const std::string role = json["message"].value("role", "");
            if (role == "user" || role == "assistant") {
                return true;
            }
        }
    }
    return false;
}

Result<void> SessionManager::writeNewFile() {
    if (m_files.exists(*m_sessionFile)) {
        return std::unexpected(Error{"EEXIST", "EEXIST: file already exists, open '" + *m_sessionFile + "'"});
    }
    return rewriteFile();
}

Result<void> SessionManager::persist(const Json& body) {
    if (!m_persist || !m_sessionFile) {
        return {};
    }
    if (!m_flushed) {
        if (!hasConversation()) {
            return {};
        }
        if (auto written = writeNewFile(); !written) {
            return written;
        }
        m_flushed = true;
        return {};
    }
    return m_files.appendFile(*m_sessionFile, m_entries.line(body));
}

std::string SessionManager::newEntryId() {
    return m_shortIds.next([&](const std::string& id) { return m_byId.contains(id); });
}

Json SessionManager::baseEntry(const std::string& type) {
    Json body = Json::object();
    body["type"] = type;
    body["id"] = newEntryId();
    body["parentId"] = m_leafId ? Json(*m_leafId) : Json(nullptr);
    body["timestamp"] = nowIso();
    return body;
}

Result<std::string> SessionManager::appendBody(Json body) {
    SessionEntry entry = m_entries.entryFromJson(body);
    m_fileEntries.push_back(body);
    m_byId[entry.id] = entry;
    m_leafId = entry.id;
    if (auto persisted = persist(body); !persisted) {
        return std::unexpected(persisted.error());
    }
    return entry.id;
}

Result<std::string> SessionManager::appendMessage(const AgentMessage& message) {
    Json body = baseEntry("message");
    body["message"] = m_messages.toJson(message);
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendThinkingLevelChange(const std::string& thinkingLevel) {
    Json body = baseEntry("thinking_level_change");
    body["thinkingLevel"] = thinkingLevel;
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendModelChange(const std::string& provider,
                                                      const std::string& modelId) {
    Json body = baseEntry("model_change");
    body["provider"] = provider;
    body["modelId"] = modelId;
    return appendBody(std::move(body));
}

Json SessionManager::usageJson(const Usage& usage) const {
    return m_codec.toJson(usage);
}

Result<SessionEntry> SessionManager::appendUsage(const std::string& kind, const std::string& provider,
                                                 const std::string& model, const Usage& usage,
                                                 const std::optional<std::string>& note) {
    Json body = baseEntry("usage");
    body["kind"] = kind;
    body["provider"] = provider;
    body["model"] = model;
    body["usage"] = usageJson(usage);
    if (note && !note->empty()) {
        body["note"] = *note;
    }
    auto id = appendBody(body);
    if (!id) {
        return std::unexpected(id.error());
    }
    return m_entries.entryFromJson(body);
}

Result<std::string> SessionManager::appendCompaction(const std::string& summary,
                                                     const std::optional<std::string>& firstKeptEntryId,
                                                     std::int64_t tokensBefore, const Json& details,
                                                     std::optional<bool> fromHook,
                                                     const std::optional<Usage>& usage) {
    Json body = baseEntry("compaction");
    const std::string id = body["id"].get<std::string>();
    body["summary"] = summary;
    body["firstKeptEntryId"] = firstKeptEntryId ? *firstKeptEntryId : id;
    body["tokensBefore"] = tokensBefore;
    if (!details.is_null()) {
        body["details"] = details;
    }
    if (usage) {
        body["usage"] = usageJson(*usage);
    }
    if (fromHook) {
        body["fromHook"] = *fromHook;
    }
    std::vector<Message> messages;
    for (const auto& message : buildSessionProjection().messages) {
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<T, CustomMessage>) {
                    messages.emplace_back(value);
                }
            },
            message);
    }
    if (auto system = m_normalizer.currentSystemMessage(messages)) {
        system->timestamp = m_iso.parse(body["timestamp"].get<std::string>()).value_or(0);
        body["systemMessage"] = m_codec.toJson(*system);
    }
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendCustomEntry(const std::string& customType, const Json& data) {
    Json body = Json::object();
    body["type"] = "custom";
    body["customType"] = customType;
    if (!data.is_null()) {
        body["data"] = data;
    }
    Json base = baseEntry("custom");
    body["id"] = base["id"];
    body["parentId"] = base["parentId"];
    body["timestamp"] = base["timestamp"];
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendSessionInfo(const std::string& name) {
    std::string sanitized;
    for (const char c : name) {
        sanitized.push_back(c == '\r' || c == '\n' ? ' ' : c);
    }
    const auto first = sanitized.find_first_not_of(" \t\r\n");
    sanitized = first == std::string::npos ? "" : sanitized.substr(first, sanitized.find_last_not_of(" \t\r\n") - first + 1);
    Json body = baseEntry("session_info");
    body["name"] = sanitized;
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendCustomMessageEntry(const std::string& customType,
                                                             const Json& content, bool display,
                                                             const Json& details) {
    Json body = Json::object();
    Json base = baseEntry("custom_message");
    body["type"] = "custom_message";
    body["customType"] = customType;
    body["content"] = content;
    body["display"] = display;
    if (!details.is_null()) {
        body["details"] = details;
    }
    body["id"] = base["id"];
    body["parentId"] = base["parentId"];
    body["timestamp"] = base["timestamp"];
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendContextEdit(const std::string& targetId,
                                                      const std::optional<Json>& replacement) {
    if (replacement && (!replacement->is_object() || !replacement->contains("content") ||
                        (!(*replacement)["content"].is_string() && !(*replacement)["content"].is_array()))) {
        return std::unexpected(Error{"invalid_edit", "Context edit replacement must be null or contain string/array content"});
    }
    const auto target = m_byId.find(targetId);
    if (target == m_byId.end()) {
        return std::unexpected(Error{"not_found", "Entry " + targetId + " not found"});
    }
    const auto path = branchPath(std::nullopt);
    if (std::none_of(path.begin(), path.end(), [&](const SessionEntry& e) { return e.id == targetId; })) {
        return std::unexpected(Error{"not_on_branch", "Entry " + targetId + " is not on the active branch"});
    }
    std::string role = "custom";
    bool editable = target->second.type == "custom_message";
    if (target->second.type == "message" && target->second.body.contains("message") &&
        target->second.body["message"].is_object()) {
        role = target->second.body["message"].value("role", "");
        editable = role == "user" || role == "assistant" || role == "toolResult";
    }
    if (!editable) {
        return std::unexpected(Error{"not_editable", "Entry " + targetId + " does not contribute editable model content"});
    }
    Json body = baseEntry("context_edit");
    body["targetId"] = targetId;
    if (!replacement) {
        body["replacement"] = nullptr;
    } else if ((role == "assistant" || role == "toolResult") && (*replacement)["content"].is_string()) {
        Json block = Json::object();
        block["type"] = "text";
        block["text"] = (*replacement)["content"];
        Json normalized = Json::object();
        normalized["content"] = Json::array({block});
        body["replacement"] = std::move(normalized);
    } else {
        body["replacement"] = *replacement;
    }
    return appendBody(std::move(body));
}

Result<std::string> SessionManager::appendLabelChange(const std::string& targetId,
                                                      const std::optional<std::string>& label) {
    if (!m_byId.contains(targetId)) {
        return std::unexpected(Error{"not_found", "Entry " + targetId + " not found"});
    }
    Json body = baseEntry("label");
    body["targetId"] = targetId;
    if (label) {
        body["label"] = *label;
    }
    const std::string timestamp = body["timestamp"].get<std::string>();
    auto id = appendBody(std::move(body));
    if (!id) {
        return id;
    }
    if (label && !label->empty()) {
        m_labels[targetId] = *label;
        m_labelTimestamps[targetId] = timestamp;
    } else {
        m_labels.erase(targetId);
        m_labelTimestamps.erase(targetId);
    }
    return id;
}

std::string SessionManager::cwd() const {
    return m_cwd;
}

std::string SessionManager::sessionDir() const {
    return m_sessionDir;
}

std::string SessionManager::sessionId() const {
    return m_sessionId;
}

std::optional<std::string> SessionManager::sessionFile() const {
    return m_sessionFile;
}

bool SessionManager::isPersisted() const {
    return m_persist;
}

std::optional<std::string> SessionManager::leafId() const {
    return m_leafId;
}

std::optional<SessionEntry> SessionManager::leafEntry() const {
    return m_leafId ? entry(*m_leafId) : std::nullopt;
}

std::optional<SessionEntry> SessionManager::entry(const std::string& id) const {
    const auto found = m_byId.find(id);
    if (found == m_byId.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<SessionEntry> SessionManager::sessionEntries() const {
    std::vector<SessionEntry> out;
    for (const auto& json : m_fileEntries) {
        if (!m_entries.isHeader(json)) {
            out.push_back(m_entries.entryFromJson(json));
        }
    }
    return out;
}

std::vector<SessionEntry> SessionManager::entries() const {
    return sessionEntries();
}

std::size_t SessionManager::entryCount() const {
    return m_byId.size();
}

std::vector<SessionEntry> SessionManager::children(const std::string& parentId) const {
    std::vector<SessionEntry> out;
    for (const auto& json : m_fileEntries) {
        if (m_entries.isHeader(json)) {
            continue;
        }
        SessionEntry candidate = m_entries.entryFromJson(json);
        if (candidate.parentId == parentId) {
            out.push_back(std::move(candidate));
        }
    }
    return out;
}

std::optional<std::string> SessionManager::label(const std::string& id) const {
    const auto found = m_labels.find(id);
    if (found == m_labels.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<SessionEntry> SessionManager::branchPath(const std::optional<std::string>& fromId) const {
    std::vector<SessionEntry> path;
    std::optional<std::string> current = fromId ? fromId : m_leafId;
    std::set<std::string> seen;
    while (current && seen.insert(*current).second) {
        const auto found = m_byId.find(*current);
        if (found == m_byId.end()) {
            break;
        }
        path.push_back(found->second);
        current = found->second.parentId;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<SessionEntry> SessionManager::buildContextEntries() const {
    return m_projector.buildContextEntries(sessionEntries(), m_leafId);
}

SessionProjection SessionManager::buildSessionProjection() const {
    return m_projector.project(sessionEntries(), m_leafId);
}

SessionContext SessionManager::buildSessionContext() const {
    return m_projector.context(sessionEntries(), m_leafId);
}

std::optional<SessionHeader> SessionManager::header() const {
    for (const auto& json : m_fileEntries) {
        if (m_entries.isHeader(json)) {
            return m_entries.headerFromJson(json);
        }
    }
    return std::nullopt;
}

std::optional<std::string> SessionManager::sessionName() const {
    for (std::size_t i = m_fileEntries.size(); i-- > 0;) {
        const Json& json = m_fileEntries[i];
        if (json.value("type", "") != "session_info") {
            continue;
        }
        std::string name = json.contains("name") && json["name"].is_string() ? json["name"].get<std::string>() : "";
        const auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::nullopt;
        }
        return name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    }
    return std::nullopt;
}

std::vector<SessionTreeNode> SessionManager::tree() const {
    const std::vector<SessionEntry> all = sessionEntries();
    std::map<std::string, std::vector<std::string>> childIds;
    std::vector<std::string> roots;
    for (const auto& entry : all) {
        if (!entry.parentId || *entry.parentId == entry.id || !m_byId.contains(*entry.parentId)) {
            roots.push_back(entry.id);
        } else {
            childIds[*entry.parentId].push_back(entry.id);
        }
    }
    const std::function<SessionTreeNode(const std::string&)> build = [&](const std::string& id) {
        SessionTreeNode node;
        node.entry = m_byId.at(id);
        node.label = label(id);
        if (const auto stamp = m_labelTimestamps.find(id); stamp != m_labelTimestamps.end()) {
            node.labelTimestamp = stamp->second;
        }
        std::vector<std::string> ids = childIds[id];
        std::stable_sort(ids.begin(), ids.end(), [&](const std::string& left, const std::string& right) {
            return m_iso.parse(m_byId.at(left).timestamp).value_or(0) <
                   m_iso.parse(m_byId.at(right).timestamp).value_or(0);
        });
        for (const auto& child : ids) {
            node.children.push_back(build(child));
        }
        return node;
    };
    std::vector<SessionTreeNode> out;
    for (const auto& root : roots) {
        out.push_back(build(root));
    }
    return out;
}

Result<void> SessionManager::branch(const std::string& branchFromId) {
    if (!m_byId.contains(branchFromId)) {
        return std::unexpected(Error{"not_found", "Entry " + branchFromId + " not found"});
    }
    m_leafId = branchFromId;
    return {};
}

void SessionManager::resetLeaf() {
    m_leafId.reset();
}

Result<std::string> SessionManager::branchWithSummary(const std::optional<std::string>& branchFromId,
                                                      const std::string& summary, const Json& details,
                                                      std::optional<bool> fromHook,
                                                      const std::optional<Usage>& usage) {
    if (branchFromId && !m_byId.contains(*branchFromId)) {
        return std::unexpected(Error{"not_found", "Entry " + *branchFromId + " not found"});
    }
    const std::string fromId = m_leafId ? *m_leafId : "root";
    m_leafId = branchFromId;
    Json body = baseEntry("branch_summary");
    body["fromId"] = fromId;
    body["summary"] = summary;
    if (!details.is_null()) {
        body["details"] = details;
    }
    if (usage) {
        body["usage"] = usageJson(*usage);
    }
    if (fromHook) {
        body["fromHook"] = *fromHook;
    }
    return appendBody(std::move(body));
}

Result<std::optional<std::string>> SessionManager::createBranchedSession(const std::string& leafId) {
    const std::optional<std::string> previousFile = m_sessionFile;
    const std::vector<SessionEntry> path = branchPath(leafId);
    if (path.empty()) {
        return std::unexpected(Error{"not_found", "Entry " + leafId + " not found"});
    }
    // Labels are real tree entries; drop them from the path (they are recreated below) and
    // re-chain the retained entries so no subtree is orphaned.
    std::vector<Json> kept;
    std::map<std::string, std::string> replacementByLabel;
    std::vector<std::string> pendingLabels;
    std::optional<std::string> pathParent;
    for (const auto& entry : path) {
        if (entry.type == "label") {
            pendingLabels.push_back(entry.id);
            continue;
        }
        for (const auto& labelId : pendingLabels) {
            replacementByLabel[labelId] = entry.id;
        }
        pendingLabels.clear();
        Json body = entry.body;
        body["parentId"] = pathParent ? Json(*pathParent) : Json(nullptr);
        if (entry.type == "compaction") {
            const std::string firstKept = entry.body.value("firstKeptEntryId", "");
            if (firstKept != entry.id && replacementByLabel.contains(firstKept)) {
                body["firstKeptEntryId"] = replacementByLabel[firstKept];
            }
        }
        kept.push_back(std::move(body));
        pathParent = entry.id;
    }
    const std::string newId = m_ids.next();
    const std::string timestamp = nowIso();
    const std::string newFile = fileFor(timestamp, newId);
    Json newHeader = makeHeader(newId, timestamp, m_persist ? previousFile : std::nullopt);

    std::set<std::string> taken;
    for (const auto& body : kept) {
        taken.insert(body.value("id", ""));
    }
    std::vector<Json> labelEntries;
    std::optional<std::string> parent = kept.empty() ? std::nullopt : std::optional<std::string>(kept.back().value("id", ""));
    for (const auto& [targetId, text] : m_labels) {
        if (!taken.contains(targetId)) {
            continue;
        }
        const std::string id = m_shortIds.next([&](const std::string& candidate) { return taken.contains(candidate); });
        taken.insert(id);
        Json labelEntry = Json::object();
        labelEntry["type"] = "label";
        labelEntry["id"] = id;
        labelEntry["parentId"] = parent ? Json(*parent) : Json(nullptr);
        labelEntry["timestamp"] = m_labelTimestamps[targetId];
        labelEntry["targetId"] = targetId;
        labelEntry["label"] = text;
        labelEntries.push_back(std::move(labelEntry));
        parent = id;
    }
    m_fileEntries = {newHeader};
    for (auto& body : kept) {
        m_fileEntries.push_back(std::move(body));
    }
    for (auto& body : labelEntries) {
        m_fileEntries.push_back(std::move(body));
    }
    m_sessionId = newId;
    if (!m_persist) {
        buildIndex();
        return std::optional<std::string>();
    }
    m_sessionFile = newFile;
    buildIndex();
    if (hasConversation()) {
        if (auto rewritten = rewriteFile(); !rewritten) {
            return std::unexpected(rewritten.error());
        }
        m_flushed = true;
    } else {
        m_flushed = false;
    }
    return std::optional<std::string>(newFile);
}
