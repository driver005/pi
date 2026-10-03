module;

#include <cstdint>

export module pi.durable.jsonl_storage;

import std;
export import pi.durable.i_staged_storage;
export import pi.platform.i_file_system;
export import pi.types.jsonl_storage_options;
import pi.support.jsonl_record_validator;
import pi.support.utf8_validator;
import pi.types.jsonl_encoded_commit;
import pi.types.jsonl_recovery_state;

/**
 * Portable JSONL implementation of the storage contract. `main.jsonl` holds one commit marker per
 * commit (conversations, entries, submissions, terminal tasks, retirements, and pointers to sidecar
 * records); `doc-<id>.jsonl` and `task-<id>.jsonl` sidecars hold document content and live task
 * records and are written before the marker, so the marker is the commit point. All reads are served
 * by an in-memory staged storage rebuilt from the files on open; recovery drops torn tails and
 * unconfirmed sidecar records and reclaims sidecars that retired documents, newer bases and
 * terminal tasks made obsolete. A failed write poisons the storage until it is reopened. Errors:
 * "jsonl_corruption", "jsonl_poisoned" and "jsonl_io" besides the storage errors of the memory store.
 * Port of packages/durable/src/storage/jsonl/storage.ts.
 */
export class JsonlStorage : public IStorage {
public:
    /** `memory` must be empty and is owned by the caller; it serves every read. */
    JsonlStorage(IStagedStorage& memory, IFileSystem& files, std::string directory, JsonlStorageOptions options = {})
        : m_memory(memory),
          m_files(files),
          m_directory(std::move(directory)),
          m_mainPath(m_directory + "/main.jsonl"),
          m_options(options) {}

    /** Creates the directory if needed and replays the files; call once before use. */
    Result<void> open() {
        const std::lock_guard<std::mutex> lock(m_commitMutex);
        if (auto created = m_files.createDirectories(m_directory); !created) {
            return std::unexpected(io("directory creation", created.error()));
        }
        return recover();
    }

    Result<std::int64_t> commit(const std::vector<Json>& writes) override {
        const std::lock_guard<std::mutex> lock(m_commitMutex);
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        auto prepared = m_memory.prepareCommit(writes, std::nullopt);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        const JsonlEncodedCommit encoded = encodeCommit(prepared->seq, prepared->writes);
        const std::map<std::string, std::string> reclamations = planReclamations(prepared->writes, encoded);
        for (const auto& [file, content] : encoded.sidecars) {
            if (auto appended = m_files.appendFile(path(file), content); !appended) {
                return std::unexpected(poison(io("append to " + file, appended.error())));
            }
        }
        if (m_options.fsync) {
            for (const auto& entry : encoded.sidecars) {
                if (auto flushed = m_files.flushFile(path(entry.first)); !flushed) {
                    return std::unexpected(poison(io("flush of " + entry.first, flushed.error())));
                }
            }
        }
        if (auto marker = m_files.appendFile(m_mainPath, encoded.marker); !marker) {
            return std::unexpected(poison(io("append to main.jsonl", marker.error())));
        }
        auto seq = m_memory.applyCommit(*prepared);
        adoptSidecarState(prepared->writes);
        reclaimSidecars(reclamations);
        return seq;
    }

    Result<std::int64_t> mintId() override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.mintId();
    }

    Result<std::optional<Json>> conversation(std::int64_t id) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.conversation(id);
    }

    Result<StoragePage> scanConversations(const ConversationQuery& query, std::size_t limit,
                                          const std::optional<Json>& cursor) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.scanConversations(query, limit, cursor);
    }

    Result<std::optional<EntryLookup>> entry(std::int64_t id) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.entry(id);
    }

    Result<std::optional<EntryLookup>> visibleEntry(std::int64_t conversationId, std::int64_t id) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.visibleEntry(conversationId, id);
    }

    Result<std::optional<Json>> findLatestHeadMarker(std::int64_t conversationId,
                                                     const std::optional<std::int64_t>& atOrBeforeEntryId) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.findLatestHeadMarker(conversationId, atOrBeforeEntryId);
    }

    Result<StoragePage> scanEntries(const EntryQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.scanEntries(query, limit, cursor);
    }

    Result<std::optional<Json>> task(std::int64_t id) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.task(id);
    }

    Result<StoragePage> scanTasks(const TaskQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.scanTasks(query, limit, cursor);
    }

    Result<std::optional<Json>> submission(std::int64_t id) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.submission(id);
    }

    Result<StoragePage> scanSubmissions(const SubmissionQuery& query, std::size_t limit,
                                        const std::optional<Json>& cursor) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.scanSubmissions(query, limit, cursor);
    }

    Result<std::optional<Json>> submissionByRequest(std::int64_t conversationId, const std::string& requestId) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.submissionByRequest(conversationId, requestId);
    }

    Result<std::optional<Json>> findDocument(const DocumentAddress& address, const DocumentPoint& at) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.findDocument(address, at);
    }

    Result<std::optional<StoredDocument>> document(std::int64_t id, const DocumentPoint& at) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.document(id, at);
    }

    Result<StoragePage> scanDocuments(const DocumentQuery& query, std::size_t limit,
                                      const std::optional<Json>& cursor) override {
        if (auto usable = checkUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        return m_memory.scanDocuments(query, limit, cursor);
    }

    Result<void> close() override {
        const std::lock_guard<std::mutex> lock(m_commitMutex);
        if (m_closed) {
            return {};
        }
        m_closed = true;
        return m_memory.close();
    }

private:
    static constexpr std::string_view kReclaimSuffix = ".reclaim";

    Error corrupt(const std::string& message) const {
        return Error{"jsonl_corruption", message};
    }

    Error io(const std::string& action, const Error& cause) const {
        return Error{"jsonl_io", "JSONL " + action + " failed: " + cause.message};
    }

    Error poison(const Error& cause) {
        if (!m_poisoned) {
            m_poison = Error{"jsonl_poisoned", "JSONL storage is poisoned and must be reopened: " + cause.message};
            m_poisoned = true;
        }
        return m_poison;
    }

    Result<void> checkUsable() const {
        if (m_closed) {
            return std::unexpected(Error{"storage_error", "JsonlStorage is closed"});
        }
        if (m_poisoned) {
            return std::unexpected(m_poison);
        }
        return {};
    }

    std::string path(const std::string& file) const {
        return m_directory + "/" + file;
    }

    std::string sidecarFileName(const std::string& kind, std::int64_t id) const {
        return kind + "-" + std::to_string(id) + ".jsonl";
    }

    std::string sidecarKey(const std::string& file, std::int64_t seq, std::int64_t ordinal) const {
        return Json::array({file, seq, ordinal}).dump();
    }

    bool isCurrentOnly(const Json& record) const {
        return record["scope"]["kind"] != "conversation" || record["history"] == "latest";
    }

    bool matchesSidecarName(const std::string& name, const std::string& suffix) const {
        if (!name.ends_with(suffix)) {
            return false;
        }
        return std::regex_match(name.substr(0, name.size() - suffix.size()), m_sidecarName);
    }

    std::string jsonLine(const Json& value) const {
        return value.dump() + "\n";
    }

    // ---------------------------------------------------------------- encoding

    JsonlEncodedCommit encodeCommit(std::int64_t seq, const std::vector<Json>& writes) const {
        Json mainWrites = Json::array();
        std::map<std::string, std::vector<Json>> records;
        std::int64_t nextOrdinal = 0;
        auto addSidecar = [&](const std::string& file, const Json& payload) {
            const std::int64_t ordinal = nextOrdinal++;
            records[file].push_back(Json{{"format", JsonlRecordValidator::kFormatVersion},
                                         {"type", "record"},
                                         {"seq", seq},
                                         {"ordinal", ordinal},
                                         {"payload", payload}});
            return ordinal;
        };
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            if (type == "conversation" || type == "entry" || type == "submission" || type == "document.retire") {
                mainWrites.push_back(write);
            } else if (type == "task") {
                const Json& value = write["value"];
                if (value["state"]["status"] == "terminal") {
                    mainWrites.push_back(write);
                } else {
                    const std::int64_t ordinal = addSidecar(sidecarFileName("task", value["id"].get<std::int64_t>()),
                                                            Json{{"type", "task"}, {"value", value}});
                    mainWrites.push_back(Json{{"type", "task.sidecar"}, {"id", value["id"]}, {"ordinal", ordinal}});
                }
            } else if (type == "document.create") {
                const std::int64_t id = write["record"]["id"].get<std::int64_t>();
                const std::int64_t ordinal = addSidecar(
                    sidecarFileName("doc", id), Json{{"type", "document"}, {"id", id}, {"content", write["content"]}});
                mainWrites.push_back(
                    Json{{"type", "document.create"}, {"record", write["record"]}, {"ordinal", ordinal}});
            } else if (type == "document.change") {
                const std::int64_t id = write["id"].get<std::int64_t>();
                const std::int64_t ordinal = addSidecar(
                    sidecarFileName("doc", id), Json{{"type", "document"}, {"id", id}, {"content", write["content"]}});
                mainWrites.push_back(Json{{"type", "document.change"}, {"id", id}, {"ordinal", ordinal}});
            }
        }
        JsonlEncodedCommit encoded;
        for (const auto& [file, fileRecords] : records) {
            std::string text;
            for (const Json& record : fileRecords) {
                text += jsonLine(record);
            }
            encoded.sidecars[file] = std::move(text);
        }
        encoded.marker = jsonLine(Json{{"format", JsonlRecordValidator::kFormatVersion},
                                       {"type", "commit"},
                                       {"seq", seq},
                                       {"writes", std::move(mainWrites)}});
        return encoded;
    }

    std::map<std::string, std::string> planReclamations(const std::vector<Json>& writes,
                                                        const JsonlEncodedCommit& encoded) const {
        std::set<std::int64_t> createdCurrentOnly;
        std::set<std::int64_t> retired;
        std::set<std::int64_t> bases;
        std::map<std::int64_t, Json> finalTasks;
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            if (type == "document.create") {
                if (isCurrentOnly(write["record"])) {
                    createdCurrentOnly.insert(write["record"]["id"].get<std::int64_t>());
                }
            } else if (type == "document.change") {
                if (write["content"]["kind"] == "base") {
                    bases.insert(write["id"].get<std::int64_t>());
                }
            } else if (type == "document.retire") {
                retired.insert(write["id"].get<std::int64_t>());
            } else if (type == "task") {
                finalTasks[write["value"]["id"].get<std::int64_t>()] = write["value"];
            }
        }
        auto currentOnlyDocument = [&](std::int64_t id) {
            return m_currentOnlyDocuments.contains(id) || createdCurrentOnly.contains(id);
        };
        std::map<std::string, std::string> replacements;
        for (const std::int64_t id : retired) {
            if (currentOnlyDocument(id)) {
                replacements[sidecarFileName("doc", id)] = "";
            }
        }
        for (const std::int64_t id : bases) {
            if (!currentOnlyDocument(id) || retired.contains(id)) {
                continue;
            }
            const std::string file = sidecarFileName("doc", id);
            const auto content = encoded.sidecars.find(file);
            if (content != encoded.sidecars.end()) {
                replacements[file] = content->second;
            }
        }
        for (const auto& [id, task] : finalTasks) {
            if (task["state"]["status"] == "terminal" &&
                (m_liveTaskSidecars.contains(id) || encoded.sidecars.contains(sidecarFileName("task", id)))) {
                replacements[sidecarFileName("task", id)] = "";
            }
        }
        return replacements;
    }

    void adoptSidecarState(const std::vector<Json>& writes) {
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            if (type == "document.create") {
                if (isCurrentOnly(write["record"])) {
                    m_currentOnlyDocuments.insert(write["record"]["id"].get<std::int64_t>());
                }
            } else if (type == "task") {
                const std::int64_t id = write["value"]["id"].get<std::int64_t>();
                if (write["value"]["state"]["status"] == "terminal") {
                    m_liveTaskSidecars.erase(id);
                } else {
                    m_liveTaskSidecars.insert(id);
                }
            }
        }
    }

    /** The marker already published this state, so reclamation is retryable best-effort maintenance. */
    void reclaimSidecars(const std::map<std::string, std::string>& replacements) {
        if (replacements.empty()) {
            return;
        }
        if (m_options.fsync && !m_files.flushFile(m_mainPath)) {
            return;
        }
        for (const auto& [file, content] : replacements) {
            replaceSidecar(file, content);
        }
    }

    void replaceSidecar(const std::string& file, const std::string& content) {
        if (content.empty()) {
            m_files.removeFile(path(file));
            return;
        }
        const std::string temporary = path(file) + std::string(kReclaimSuffix);
        if (!m_files.writeFile(temporary, content)) {
            return;
        }
        if (m_options.fsync && !m_files.flushFile(temporary)) {
            return;
        }
        m_files.renameFile(temporary, path(file));
    }

    // ---------------------------------------------------------------- recovery

    Result<JsonlFile> readLines(const std::string& filePath, const std::string& name,
                                const std::function<Result<Json>(const std::string&, std::size_t)>& parse) {
        JsonlFile parsed;
        parsed.path = filePath;
        auto read = m_files.readFile(filePath);
        if (!read) {
            if (read.error().code == "ENOENT") {
                return parsed;
            }
            return std::unexpected(io("read of " + name, read.error()));
        }
        const std::string& bytes = *read;
        std::size_t complete = bytes.size();
        if (complete > 0 && bytes.back() != '\n') {
            complete = bytes.rfind('\n') == std::string::npos ? 0 : bytes.rfind('\n') + 1;
            if (auto truncated = m_files.truncateFile(filePath, complete); !truncated) {
                return std::unexpected(io("torn-line truncation of " + name, truncated.error()));
            }
        }
        std::size_t start = 0;
        std::size_t lineNumber = 1;
        for (std::size_t end = 0; end < complete; ++end) {
            if (bytes[end] != '\n') {
                continue;
            }
            const std::string text = bytes.substr(start, end - start);
            if (!m_utf8.valid(text)) {
                return std::unexpected(corrupt("Invalid UTF-8 in complete " + name + " line " + std::to_string(lineNumber)));
            }
            auto value = parse(text, lineNumber);
            if (!value) {
                return std::unexpected(value.error());
            }
            parsed.lines.push_back(JsonlLine{std::move(*value), start});
            start = end + 1;
            ++lineNumber;
        }
        return parsed;
    }

    Result<void> recover() {
        JsonlRecoveryState state;
        if (auto loaded = loadMain(state); !loaded) {
            return loaded;
        }
        if (auto loaded = loadSidecars(state); !loaded) {
            return loaded;
        }
        classify(state);
        findLatestBases(state);
        if (auto replayed = replay(state); !replayed) {
            return replayed;
        }
        if (auto settled = settleSidecars(state); !settled) {
            return settled;
        }
        for (const std::int64_t id : state.currentOnlyDocuments) {
            m_currentOnlyDocuments.insert(id);
        }
        for (const auto& [id, live] : state.finalTaskIsLive) {
            if (live) {
                m_liveTaskSidecars.insert(id);
            }
        }
        return {};
    }

    Result<void> loadMain(JsonlRecoveryState& state) {
        auto main = readLines(m_mainPath, "main.jsonl", [&](const std::string& text, std::size_t line) {
            return m_validator.parseMainMarker(text, line);
        });
        if (!main) {
            return std::unexpected(main.error());
        }
        std::int64_t previous = 0;
        for (const JsonlLine& line : main->lines) {
            const std::int64_t seq = line.value["seq"].get<std::int64_t>();
            if (seq <= previous) {
                return std::unexpected(corrupt("Commit sequence does not strictly increase in main.jsonl"));
            }
            previous = seq;
        }
        state.main = std::move(*main);
        return {};
    }

    Result<void> loadSidecars(JsonlRecoveryState& state) {
        auto listed = m_files.listDirectory(m_directory);
        if (!listed) {
            return std::unexpected(io("directory listing", listed.error()));
        }
        std::vector<std::string> names;
        for (const std::string& name : *listed) {
            if (matchesSidecarName(name, std::string(kReclaimSuffix))) {
                m_files.removeFile(path(name));
            } else if (matchesSidecarName(name, "")) {
                names.push_back(name);
            }
        }
        std::ranges::sort(names);
        for (const std::string& file : names) {
            auto parsed = readLines(path(file), file, [&](const std::string& text, std::size_t line) {
                return m_validator.parseSidecarRecord(text, file, line);
            });
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            const Json* previous = nullptr;
            for (const JsonlLine& line : parsed->lines) {
                const std::int64_t seq = line.value["seq"].get<std::int64_t>();
                const std::int64_t ordinal = line.value["ordinal"].get<std::int64_t>();
                if (previous != nullptr) {
                    const std::int64_t previousSeq = (*previous)["seq"].get<std::int64_t>();
                    const std::int64_t previousOrdinal = (*previous)["ordinal"].get<std::int64_t>();
                    if (seq < previousSeq || (seq == previousSeq && ordinal <= previousOrdinal)) {
                        return std::unexpected(corrupt("Sidecar records are out of order in " + file));
                    }
                }
                previous = &line.value;
                state.recordByKey[sidecarKey(file, seq, ordinal)] = line.value;
            }
            state.sidecarFiles[file] = std::move(*parsed);
        }
        return {};
    }

    void classify(JsonlRecoveryState& state) const {
        std::set<std::int64_t> retired;
        for (const JsonlLine& line : state.main.lines) {
            for (const Json& operation : line.value["writes"]) {
                const std::string type = operation["type"].get<std::string>();
                if (type == "document.create") {
                    if (isCurrentOnly(operation["record"])) {
                        state.currentOnlyDocuments.insert(operation["record"]["id"].get<std::int64_t>());
                    }
                } else if (type == "document.retire") {
                    retired.insert(operation["id"].get<std::int64_t>());
                } else if (type == "task") {
                    state.finalTaskIsLive[operation["value"]["id"].get<std::int64_t>()] = false;
                } else if (type == "task.sidecar") {
                    state.finalTaskIsLive[operation["id"].get<std::int64_t>()] = true;
                }
            }
        }
        for (const std::int64_t id : retired) {
            if (state.currentOnlyDocuments.contains(id)) {
                state.retiredCurrentOnlyDocuments.insert(id);
            }
        }
        for (const auto& [id, live] : state.finalTaskIsLive) {
            if (!live) {
                state.terminalTasks.insert(id);
            }
        }
    }

    void findLatestBases(JsonlRecoveryState& state) const {
        for (const JsonlLine& line : state.main.lines) {
            const std::int64_t seq = line.value["seq"].get<std::int64_t>();
            for (const Json& operation : line.value["writes"]) {
                const std::string type = operation["type"].get<std::string>();
                if (type != "document.create" && type != "document.change") {
                    continue;
                }
                const std::int64_t id = type == "document.create" ? operation["record"]["id"].get<std::int64_t>()
                                                                  : operation["id"].get<std::int64_t>();
                if (!state.currentOnlyDocuments.contains(id)) {
                    continue;
                }
                const auto found = state.recordByKey.find(
                    sidecarKey(sidecarFileName("doc", id), seq, operation["ordinal"].get<std::int64_t>()));
                if (found == state.recordByKey.end()) {
                    continue;
                }
                const Json& record = found->second;
                if (record["payload"]["type"] != "document" || record["payload"]["id"] != id ||
                    record["payload"]["content"]["kind"] != "base") {
                    continue;
                }
                const auto previous = state.latestBases.find(id);
                if (previous == state.latestBases.end() || isNewer(record, previous->second)) {
                    state.latestBases[id] = record;
                }
            }
        }
    }

    bool isNewer(const Json& record, const Json& than) const {
        const std::int64_t seq = record["seq"].get<std::int64_t>();
        const std::int64_t thanSeq = than["seq"].get<std::int64_t>();
        return seq > thanSeq || (seq == thanSeq && record["ordinal"].get<std::int64_t>() > than["ordinal"].get<std::int64_t>());
    }

    bool isBeforeLatestBase(const JsonlRecoveryState& state, std::int64_t id, std::int64_t seq, std::int64_t ordinal) const {
        const auto base = state.latestBases.find(id);
        if (base == state.latestBases.end()) {
            return false;
        }
        const std::int64_t baseSeq = base->second["seq"].get<std::int64_t>();
        return seq < baseSeq || (seq == baseSeq && ordinal < base->second["ordinal"].get<std::int64_t>());
    }

    Result<std::optional<Json>> confirmRecord(JsonlRecoveryState& state, std::int64_t seq, std::int64_t ordinal,
                                              const std::string& file, bool optional) const {
        const std::string key = sidecarKey(file, seq, ordinal);
        if (state.confirmed.contains(key)) {
            return std::unexpected(corrupt("Sidecar record is confirmed more than once"));
        }
        const auto found = state.recordByKey.find(key);
        if (found == state.recordByKey.end()) {
            if (optional) {
                return std::optional<Json>();
            }
            return std::unexpected(
                corrupt("Missing confirmed sidecar record " + file + " at sequence " + std::to_string(seq)));
        }
        state.confirmed.insert(key);
        return std::optional<Json>(found->second);
    }

    Result<void> replay(JsonlRecoveryState& state) {
        for (const JsonlLine& line : state.main.lines) {
            const Json& marker = line.value;
            const std::int64_t seq = marker["seq"].get<std::int64_t>();
            std::vector<Json> writes;
            for (const Json& operation : marker["writes"]) {
                if (auto replayed = replayOperation(state, seq, operation, writes); !replayed) {
                    return replayed;
                }
            }
            auto prepared = m_memory.prepareCommit(writes, seq);
            if (prepared) {
                auto applied = m_memory.applyCommit(*prepared);
                if (applied) {
                    continue;
                }
                return std::unexpected(corrupt("Invalid committed state at sequence " + std::to_string(seq) + ": " + applied.error().message));
            }
            return std::unexpected(corrupt("Invalid committed state at sequence " + std::to_string(seq) + ": " + prepared.error().message));
        }
        return {};
    }

    Result<void> replayOperation(JsonlRecoveryState& state, std::int64_t seq, const Json& operation, std::vector<Json>& writes) {
        const std::string type = operation["type"].get<std::string>();
        if (type == "conversation" || type == "entry" || type == "submission" || type == "task" || type == "document.retire") {
            writes.push_back(operation);
            return {};
        }
        if (type == "task.sidecar") {
            const std::int64_t id = operation["id"].get<std::int64_t>();
            const bool optional = state.terminalTasks.contains(id);
            auto record = confirmRecord(state, seq, operation["ordinal"].get<std::int64_t>(), sidecarFileName("task", id), optional);
            if (!record) {
                return std::unexpected(record.error());
            }
            if (*record) {
                const Json& payload = (**record)["payload"];
                if (payload["type"] != "task" || payload["value"]["id"] != id) {
                    return std::unexpected(corrupt("Confirmed task sidecar data does not match commit " + std::to_string(seq)));
                }
                if (!optional) {
                    writes.push_back(Json{{"type", "task"}, {"value", payload["value"]}});
                }
            }
            return {};
        }
        return replayDocumentOperation(state, seq, operation, writes);
    }

    Result<void> replayDocumentOperation(JsonlRecoveryState& state, std::int64_t seq, const Json& operation,
                                         std::vector<Json>& writes) {
        const bool creating = operation["type"] == "document.create";
        const std::int64_t id = creating ? operation["record"]["id"].get<std::int64_t>() : operation["id"].get<std::int64_t>();
        const std::int64_t ordinal = operation["ordinal"].get<std::int64_t>();
        const bool reclaimed = state.retiredCurrentOnlyDocuments.contains(id) || isBeforeLatestBase(state, id, seq, ordinal);
        auto record = confirmRecord(state, seq, ordinal, sidecarFileName("doc", id), reclaimed);
        if (!record) {
            return std::unexpected(record.error());
        }
        std::optional<Json> content;
        if (*record) {
            const Json& payload = (**record)["payload"];
            if (payload["type"] != "document" || payload["id"] != id) {
                return std::unexpected(corrupt("Confirmed document sidecar data does not match commit " + std::to_string(seq)));
            }
            content = payload["content"];
        }
        if (creating) {
            if (content && (*content)["kind"] != "base") {
                return std::unexpected(corrupt("Document creation lacks a confirmed base in commit " + std::to_string(seq)));
            }
            Json body = reclaimed || !content ? Json{{"kind", "base"}, {"version", 1}, {"value", Json::object()}} : *content;
            writes.push_back(Json{{"type", "document.create"}, {"record", operation["record"]}, {"content", std::move(body)}});
        } else if (!reclaimed && content) {
            writes.push_back(Json{{"type", "document.change"}, {"id", id}, {"content", *content}});
        }
        return {};
    }

    Result<void> settleSidecars(JsonlRecoveryState& state) {
        std::map<std::string, std::string> reclamations;
        for (const auto& [file, parsed] : state.sidecarFiles) {
            std::optional<std::size_t> unconfirmedAt;
            for (const JsonlLine& line : parsed.lines) {
                const std::string key = sidecarKey(file, line.value["seq"].get<std::int64_t>(), line.value["ordinal"].get<std::int64_t>());
                if (state.confirmed.contains(key)) {
                    if (unconfirmedAt) {
                        return std::unexpected(corrupt("Confirmed record follows an unconfirmed tail in " + file));
                    }
                } else if (!unconfirmedAt) {
                    unconfirmedAt = line.start;
                }
            }
            if (unconfirmedAt) {
                if (auto truncated = m_files.truncateFile(parsed.path, *unconfirmedAt); !truncated) {
                    return std::unexpected(io("tail truncation of " + file, truncated.error()));
                }
            }
            planSidecarReclamation(state, file, parsed, reclamations);
        }
        reclaimSidecars(reclamations);
        return {};
    }

    void planSidecarReclamation(const JsonlRecoveryState& state, const std::string& file, const JsonlFile& parsed,
                                std::map<std::string, std::string>& reclamations) const {
        const std::int64_t numericId = std::stoll(file.substr(file.find('-') + 1, file.size() - file.find('-') - 7));
        std::vector<const Json*> confirmedLines;
        for (const JsonlLine& line : parsed.lines) {
            if (state.confirmed.contains(sidecarKey(file, line.value["seq"].get<std::int64_t>(), line.value["ordinal"].get<std::int64_t>()))) {
                confirmedLines.push_back(&line.value);
            }
        }
        std::optional<std::vector<const Json*>> retained;
        if (file.starts_with("task-") && state.terminalTasks.contains(numericId)) {
            retained = std::vector<const Json*>{};
        } else if (file.starts_with("doc-")) {
            if (state.retiredCurrentOnlyDocuments.contains(numericId)) {
                retained = std::vector<const Json*>{};
            } else if (state.latestBases.contains(numericId)) {
                retained = std::vector<const Json*>{};
                for (const Json* line : confirmedLines) {
                    if (!isBeforeLatestBase(state, numericId, (*line)["seq"].get<std::int64_t>(), (*line)["ordinal"].get<std::int64_t>())) {
                        retained->push_back(line);
                    }
                }
            }
        }
        if (retained && (retained->size() < confirmedLines.size() || retained->empty())) {
            std::string text;
            for (const Json* line : *retained) {
                text += jsonLine(*line);
            }
            reclamations[file] = std::move(text);
        }
    }

    IStagedStorage& m_memory;
    IFileSystem& m_files;
    std::string m_directory;
    std::string m_mainPath;
    JsonlStorageOptions m_options;
    JsonlRecordValidator m_validator;
    const std::regex m_sidecarName{R"(^(?:doc|task)-(?:0|[1-9][0-9]*)\.jsonl)"};
    Utf8Validator m_utf8;
    std::set<std::int64_t> m_currentOnlyDocuments;
    std::set<std::int64_t> m_liveTaskSidecars;
    std::mutex m_commitMutex;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_poisoned{false};
    Error m_poison;
};
