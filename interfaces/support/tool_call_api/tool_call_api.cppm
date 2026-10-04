module;

#include <cstdint>

export module pi.support.tool_call_api;

import std;
export import pi.durable.i_task_runtime;
export import pi.durable.i_tool_execution_api;
export import pi.support.builtin_documents;
export import pi.support.delta_differ;
export import pi.support.live_editor;
export import pi.support.output_buffer;
export import pi.support.progress_publisher;
export import pi.types.bounded_output;
export import pi.types.output_limits;
export import pi.types.tool_diagnostic;

/**
 * The API one tool invocation sees, and the record of what it reported: bounded running output, the last details and
 * its diagnostics, published into the call's `pi.live.tools` slot by throttled commits (spec §8.2). Port of the api
 * object and `publishProgress()` of packages/durable/src/harness/tool.ts.
 */
export class ToolCallApi : public IToolExecutionApi {
public:
    ToolCallApi(ITaskRuntime& runtime, std::string callId, const OutputLimits& limits, std::shared_ptr<IExecutionEnv> env)
        : m_runtime(runtime),
          m_callId(std::move(callId)),
          m_env(std::move(env)),
          m_output(limits),
          m_progress([this] { return publish(); },
                     [this](const Error& error) {
                         // Failures after an abort mark or close are expected; the committed state stays consistent.
                         if (!m_runtime.signal().aborted()) {
                             m_runtime.report(error);
                         }
                     }) {}

    // ─── What the call reported ─────────────────────────────────────────────

    /** Ends the running output and stops the progress commits; returns the detail waiters the final commit settles. */
    std::vector<std::shared_ptr<WaiterSlot>> finish() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_ended = true;
            m_output.end();
        }
        return m_progress.stop();
    }

    /** Stops the progress commits without ending the output, for a call aborted with its invocation. */
    std::vector<std::shared_ptr<WaiterSlot>> abandon() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_ended = true;
        }
        return m_progress.stop();
    }

    BoundedOutput retainedOutput() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_output.snapshot();
    }

    std::optional<Json> reportedDetails() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_details;
    }

    std::vector<ToolDiagnostic> reportedDiagnostics() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_diagnostics;
    }

    void resolve(const std::shared_ptr<WaiterSlot>& waiter) {
        m_progress.resolve(waiter);
    }

    void reject(const std::shared_ptr<WaiterSlot>& waiter, const Error& error) {
        m_progress.reject(waiter, error);
    }

    // ─── IToolExecutionApi ──────────────────────────────────────────────────

    std::int64_t taskId() const override {
        return m_runtime.taskId();
    }

    std::int64_t conversationId() const override {
        return m_runtime.conversationId();
    }

    std::string callId() const override {
        return m_callId;
    }

    std::shared_ptr<IExecutionEnv> env() override {
        return m_env;
    }

    AbortSignal& signal() override {
        return m_runtime.signal();
    }

    Result<void> output(const std::string& chunk) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (auto live = assertLive(); !live) {
                return live;
            }
            if (!m_output.push(chunk)) {
                return {};
            }
        }
        m_progress.mark();
        return {};
    }

    Result<void> diagnostic(const ToolDiagnostic& diagnostic) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (auto live = assertLive(); !live) {
                return live;
            }
            m_diagnostics.push_back(diagnostic);
        }
        m_progress.mark();
        return {};
    }

    Result<void> details(const Json& value) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (auto live = assertLive(); !live) {
                return live;
            }
            m_details = value;
        }
        auto waiter = m_progress.markAndWait();
        return m_progress.await(waiter);
    }

    Result<void> commit(const std::function<Result<void>(Transaction&)>& change) override {
        return m_runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            if (auto changed = change(tx); !changed) {
                return std::unexpected(changed.error());
            }
            return std::optional<Json>();
        });
    }

    Result<std::int64_t> createTask(const std::string& name, const Json& input, const TaskOptions& options) override {
        auto fresh = m_runtime.newTask(name, input);
        if (!fresh) {
            return std::unexpected(fresh.error());
        }
        std::int64_t id = 0;
        auto committed = m_runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            auto created = tx.createTask(name, fresh->at("version").get<std::int64_t>(), input, fresh->at("checkpoint"), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return std::optional<Json>();
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return id;
    }

    Result<std::optional<Json>> getTask(std::int64_t id) override {
        return m_runtime.getTask(id);
    }

    Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) override {
        return m_runtime.waitForTask(id, cancel);
    }

    Result<std::optional<Json>> memo(const std::string& name) override {
        return m_runtime.memo(name);
    }

    Result<Json> memo(const std::string& name, const Json& candidate) override {
        return m_runtime.memo(name, candidate);
    }

    Result<std::optional<Json>> snapshot(const DocDefinition& definition, const DocAddressArgs& args) override {
        return m_runtime.snapshot(definition, args);
    }

    Result<std::optional<Json>> snapshotAsOf(const DocDefinition& definition, const DocAddressArgs& args,
                                             std::int64_t entryId) override {
        return m_runtime.snapshotAsOf(definition, args, entryId);
    }

private:
    Result<void> assertLive() const {
        if (m_ended) {
            return std::unexpected(Error{"call_settled", "Tool call " + m_callId + " has settled"});
        }
        return {};
    }

    /**
     * Commits what the tool reported since the last commit into its slot and returns the bytes written. Everything is
     * captured first: the tool keeps reporting while the commit is in flight.
     */
    Result<std::int64_t> publish() {
        BoundedOutput snapshot;
        std::optional<Json> details;
        std::vector<ToolDiagnostic> added;
        std::size_t diagnosticCount = 0;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_output.snapshot();
            details = m_details;
            diagnosticCount = m_diagnostics.size();
            added.assign(m_diagnostics.begin() + static_cast<std::ptrdiff_t>(m_writtenDiagnostics), m_diagnostics.end());
        }
        const bool detailsChanged = details != m_writtenDetails;
        const std::int64_t bytes = writtenBytes(snapshot.text, details, detailsChanged, added);
        auto committed = m_runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            DocAddressArgs args;
            args.owner = m_runtime.conversationId();
            auto live = tx.doc(m_documents.live(), args);
            if (!live) {
                return std::unexpected(live.error());
            }
            Json* slot = m_live.toolSlot(**live, m_runtime.taskId());
            if (slot == nullptr) {
                return std::optional<Json>();
            }
            // Assign `output` as one string field: the diff is then an append, or a trim plus an append for a sliding tail.
            if (slot->value("output", std::string()) != snapshot.text) {
                (*slot)["output"] = snapshot.text;
            }
            if (snapshot.droppedBytes > 0) {
                (*slot)["droppedBytes"] = snapshot.droppedBytes;
            }
            if (snapshot.droppedLines > 0) {
                (*slot)["droppedLines"] = snapshot.droppedLines;
            }
            if (detailsChanged && details) {
                (*slot)["details"] = *details;
            }
            if (!added.empty() && !slot->contains("diagnostics")) {
                (*slot)["diagnostics"] = Json::array();
            }
            for (const ToolDiagnostic& item : added) {
                (*slot)["diagnostics"].push_back(Json::object({{"severity", item.severity}, {"message", item.message}}));
                if (item.code) {
                    (*slot)["diagnostics"].back()["code"] = *item.code;
                }
            }
            return std::optional<Json>();
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        m_writtenText = snapshot.text;
        m_writtenDetails = details;
        m_writtenDiagnostics = diagnosticCount;
        return bytes;
    }

    /** What the commit writes, as the delta of its string fields: an append, or a trim plus an append, or the window. */
    std::int64_t writtenBytes(const std::string& text, const std::optional<Json>& details, bool detailsChanged,
                              const std::vector<ToolDiagnostic>& added) const {
        std::int64_t bytes = 0;
        if (text != m_writtenText) {
            const Json ops = m_differ.diff(Json::object({{"o", m_writtenText}}), Json::object({{"o", text}}));
            bytes += static_cast<std::int64_t>(ops.dump().size());
        }
        if (detailsChanged) {
            bytes += static_cast<std::int64_t>((details ? details->dump() : std::string("null")).size());
        }
        for (const ToolDiagnostic& item : added) {
            bytes += static_cast<std::int64_t>(item.message.size() + item.severity.size());
        }
        return bytes;
    }

    ITaskRuntime& m_runtime;
    std::string m_callId;
    std::shared_ptr<IExecutionEnv> m_env;
    BuiltinDocuments m_documents;
    LiveEditor m_live;
    DeltaDiffer m_differ;
    std::mutex m_mutex;
    OutputBuffer m_output;
    std::optional<Json> m_details;
    std::vector<ToolDiagnostic> m_diagnostics;
    bool m_ended = false;
    std::string m_writtenText;
    std::optional<Json> m_writtenDetails;
    std::size_t m_writtenDiagnostics = 0;
    ProgressPublisher m_progress;
};
