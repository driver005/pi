export module pi.support.recovery_attempt_omitter;

import std;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.support.message_entry_locator;
export import pi.support.session_context_refresher;
export import pi.types.result;
export import pi.types.tool_result_message;

/**
 * Keeps a failed attempt in the session file but durably removes it from model context (a
 * context edit with no replacement), so a retry or post-compaction rerun does not see it. Used by
 * auto-retry and overflow recovery.
 */
export class RecoveryAttemptOmitter {
public:
    RecoveryAttemptOmitter(ISessionManager& session, ISessionEventSink& sink,
                           SessionContextRefresher& refresher);

    /** Error when the assistant message is not in the session; missing tool results are skipped. */
    Result<void> omit(const AssistantMessage& message, const std::vector<ToolResultMessage>& toolResults);

private:
    Result<void> omitEntry(const std::string& entryId);

    ISessionManager& m_session;
    ISessionEventSink& m_sink;
    SessionContextRefresher& m_refresher;
    MessageEntryLocator m_locator;
};

RecoveryAttemptOmitter::RecoveryAttemptOmitter(ISessionManager& session, ISessionEventSink& sink,
                                               SessionContextRefresher& refresher)
    : m_session(session), m_sink(sink), m_refresher(refresher) {}

Result<void> RecoveryAttemptOmitter::omitEntry(const std::string& entryId) {
    const auto edit = m_session.appendContextEdit(entryId, std::nullopt);
    if (!edit) {
        return std::unexpected(edit.error());
    }
    if (auto entry = m_session.entry(*edit)) {
        AgentSessionEvent event;
        event.type = SessionEventType::EntryAppended;
        event.entry = std::move(entry);
        m_sink.emit(event);
    }
    return {};
}

Result<void> RecoveryAttemptOmitter::omit(const AssistantMessage& message,
                                          const std::vector<ToolResultMessage>& toolResults) {
    const std::vector<SessionEntry> branch = m_session.branchPath();
    const auto messageId = m_locator.find(branch, AgentMessage(message));
    if (!messageId) {
        return std::unexpected(
            Error{"omit_failed", "Cannot persist recovery omission because the message has no source entry"});
    }
    std::vector<std::string> targets = {*messageId};
    for (const auto& result : toolResults) {
        if (const auto id = m_locator.find(branch, AgentMessage(result))) {
            targets.push_back(*id);
        }
    }
    for (const auto& target : targets) {
        if (auto omitted = omitEntry(target); !omitted) {
            return omitted;
        }
    }
    m_refresher.refresh();
    return {};
}
