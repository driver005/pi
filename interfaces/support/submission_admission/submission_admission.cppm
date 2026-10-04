module;

#include <cstdint>

export module pi.support.submission_admission;

import std;
export import pi.support.builtin_documents;
export import pi.support.entry_kinds;
export import pi.support.inbox_boundary;
export import pi.support.run_starter;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;
export import pi.types.submission_draft;

/**
 * Admits a submission inside a commit (spec §6); `Conversation.submit()` and conversation-owned compactions share it.
 * A known request id returns its existing submission without writing. A busy conversation queues it in `pi.inbox`, or
 * rejects `whenBusy: "reject"` input with `conversation_busy`. An idle conversation with queued items queues it behind
 * them and runs a final boundary. Otherwise idle input places a user entry and starts a run, and an idle write appends
 * its entry and settles `done`, or `stale` when its head reaches before the active range.
 */
export class SubmissionAdmission {
public:
    Result<std::int64_t> admit(Transaction& tx, std::int64_t conversationId, const SubmissionDraft& draft, std::int64_t now,
                               const std::string& steeringMode, const std::string& followUpMode) const {
        if (draft.requestId) {
            auto existing = tx.submissionByRequest(conversationId, *draft.requestId);
            if (!existing) {
                return std::unexpected(existing.error());
            }
            if (*existing) {
                if ((*existing)->at("type") != draft.type) {
                    return std::unexpected(Error{"durable_error", "Request " + *draft.requestId + " already identifies a submission of type " +
                                                                      (*existing)->at("type").get<std::string>()});
                }
                return (*existing)->at("id").get<std::int64_t>();
            }
        }
        DocAddressArgs args;
        args.owner = conversationId;
        auto live = tx.doc(m_documents.live(), args);
        if (!live) {
            return std::unexpected(live.error());
        }
        const bool busy = (*live)->contains("run");
        if (busy && draft.type == "input" && draft.whenBusy == std::optional<std::string>("reject")) {
            return std::unexpected(Error{"conversation_busy", "Conversation " + std::to_string(conversationId) + " is busy"});
        }
        // A boundary reads the table, so it is prepared before the first table write; a busy one needs none.
        std::optional<Boundary> boundary;
        if (!busy) {
            auto prepared = m_boundary.prepare(tx, conversationId, steeringMode, followUpMode);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            boundary = *prepared;
        }
        if (!boundary || !boundary->inbox->at("items").empty()) {
            return queue(tx, conversationId, draft, now, boundary, **live, args);
        }
        return place(tx, conversationId, draft, now, *boundary, **live);
    }

private:
    Json createFields(std::int64_t conversationId, const SubmissionDraft& draft, const std::string& status) const {
        Json fields = Json::object({{"conversationId", conversationId}});
        if (draft.requestId) {
            fields["requestId"] = *draft.requestId;
        }
        fields["type"] = draft.type;
        fields["status"] = status;
        return fields;
    }

    /** A busy conversation, or an idle one with queued items, queues the submission. */
    Result<std::int64_t> queue(Transaction& tx, std::int64_t conversationId, const SubmissionDraft& draft, std::int64_t now,
                               std::optional<Boundary>& boundary, Json& live, const DocAddressArgs& args) const {
        auto record = tx.createSubmission(createFields(conversationId, draft, "queued"));
        if (!record) {
            return std::unexpected(record.error());
        }
        const std::int64_t id = record->at("id").get<std::int64_t>();
        Json* inbox = boundary ? boundary->inbox : nullptr;
        if (inbox == nullptr) {
            auto doc = tx.doc(m_documents.inbox(), args);
            if (!doc) {
                return std::unexpected(doc.error());
            }
            inbox = *doc;
        }
        Json item = Json::object({{"id", id}});
        if (draft.type == "write") {
            item["mode"] = "write";
            item["entry"] = draft.entry;
        } else {
            item["mode"] = draft.whenBusy == std::optional<std::string>("steer") ? "steer" : "followUp";
            item["content"] = draft.content;
        }
        (*inbox)["items"].push_back(item);
        if (!boundary) {
            return id;
        }
        auto applied = m_boundary.apply(tx, *boundary, "final", now);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (!applied->users.empty()) {
            if (auto started = m_runs.startRun(tx, conversationId, live, applied->users); !started) {
                return std::unexpected(started.error());
            }
        }
        return id;
    }

    /** An idle conversation with an empty inbox places the submission at once. */
    Result<std::int64_t> place(Transaction& tx, std::int64_t conversationId, const SubmissionDraft& draft, std::int64_t now,
                               const Boundary& boundary, Json& live) const {
        if (draft.type == "write") {
            if (m_boundary.isStale(boundary, draft.entry)) {
                Json stale = createFields(conversationId, draft, "unanswered");
                stale["reason"] = "stale";
                return create(tx, stale);
            }
            auto entry = tx.appendEntry(conversationId, draft.entry);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            Json done = createFields(conversationId, draft, "done");
            done["entry"] = entry->at("id");
            return create(tx, done);
        }
        const Json message = Json::object({{"role", "user"}, {"content", draft.content}, {"timestamp", now}});
        auto entry = tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.user()}, {"model", Json::array({message})}}));
        if (!entry) {
            return std::unexpected(entry.error());
        }
        Json placed = createFields(conversationId, draft, "placed");
        placed["entry"] = entry->at("id");
        auto id = create(tx, placed);
        if (!id) {
            return id;
        }
        if (auto started = m_runs.startRun(tx, conversationId, live, {*id}); !started) {
            return std::unexpected(started.error());
        }
        return id;
    }

    Result<std::int64_t> create(Transaction& tx, const Json& fields) const {
        auto record = tx.createSubmission(fields);
        if (!record) {
            return std::unexpected(record.error());
        }
        return record->at("id").get<std::int64_t>();
    }

    BuiltinDocuments m_documents;
    EntryKinds m_kinds;
    InboxBoundary m_boundary;
    RunStarter m_runs;
};
