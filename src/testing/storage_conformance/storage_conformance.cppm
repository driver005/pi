module;

#include <cstdint>

export module pi.testing.storage_conformance;

import std;
export import pi.durable.i_storage;

/**
 * The behaviour every IStorage backend must show, port of packages/durable/src/testing/
 * storage-conformance.ts. run() makes a fresh storage per case and returns the failed expectations
 * (case name, source line, what), so a test can print them all at once. Cases that only exercise
 * JavaScript object semantics (prototype keys, lone surrogates) are left out.
 */
export class StorageConformance {
public:
    using Factory = std::function<std::shared_ptr<IStorage>()>;

    std::vector<std::string> run(const Factory& make) {
        m_failures.clear();
        const std::vector<std::pair<std::string, void (StorageConformance::*)()>> cases = {
            {"reserves ID 1 for the root conversation", &StorageConformance::reservesRoot},
            {"commits mixed writes atomically", &StorageConformance::mixedWritesAreAtomic},
            {"detaches retained writes and returned records", &StorageConformance::detachesRecords},
            {"indexes entries committed out of order", &StorageConformance::entriesOutOfOrder},
            {"continues an entry cursor after a newer commit", &StorageConformance::entryCursor},
            {"paginates conversations", &StorageConformance::conversationPages},
            {"filters conversations by owner", &StorageConformance::conversationOwners},
            {"scans deep fork history", &StorageConformance::forkHistory},
            {"replaces task records and filters scans", &StorageConformance::taskRecords},
            {"stores task owners and waiting states", &StorageConformance::taskOwners},
            {"indexes request ids per conversation", &StorageConformance::requestIds},
            {"stores passive write submissions", &StorageConformance::passiveWrites},
            {"reconstructs rewindable documents", &StorageConformance::rewindableDocuments},
            {"streams long document tails", &StorageConformance::longTails},
            {"copies document bases", &StorageConformance::documentCopies},
            {"uses bases for version transitions", &StorageConformance::versionTransitions},
            {"indexes addresses and scopes", &StorageConformance::addressesAndScopes},
            {"keeps document lifecycle failures atomic", &StorageConformance::lifecycleFailures},
            {"rolls back tables and indexes", &StorageConformance::rollsBackIndexes},
            {"keeps string identities distinct", &StorageConformance::stringIdentities},
            {"keeps one id namespace", &StorageConformance::idNamespace},
            {"rejects operations after close", &StorageConformance::rejectsAfterClose},
        };
        for (const auto& [name, test] : cases) {
            m_case = name;
            m_storage = make();
            (this->*test)();
        }
        m_storage.reset();
        return m_failures;
    }

private:
    static constexpr std::int64_t kRoot = 1;

    void check(bool condition, const std::string& what, std::source_location where = std::source_location::current()) {
        if (!condition) {
            m_failures.push_back(m_case + " (line " + std::to_string(where.line()) + "): " + what);
        }
    }

    void same(const Json& actual, const Json& expected, std::source_location where = std::source_location::current()) {
        check(actual == expected, "expected " + expected.dump() + " but got " + actual.dump(), where);
    }

    template <typename T>
    void rejectsWith(const Result<T>& result, const std::string& text,
                     std::source_location where = std::source_location::current()) {
        if (result) {
            check(false, "expected a failure containing \"" + text + "\"", where);
        } else {
            check(result.error().message.find(text) != std::string::npos,
                  "failure \"" + result.error().message + "\" lacks \"" + text + "\"", where);
        }
    }

    template <typename T>
    T value(const Result<T>& result, std::source_location where = std::source_location::current()) {
        if (!result) {
            check(false, "unexpected failure: " + result.error().message, where);
            return T();
        }
        return *result;
    }

    Json found(const Result<std::optional<Json>>& result, std::source_location where = std::source_location::current()) {
        const auto unwrapped = value(result, where);
        return unwrapped ? *unwrapped : Json();
    }

    std::int64_t commit(const std::vector<Json>& writes, std::source_location where = std::source_location::current()) {
        return value(m_storage->commit(writes), where);
    }

    std::int64_t mint() {
        return value(m_storage->mintId());
    }

    Json ids(const StoragePage& page) {
        Json out = Json::array();
        for (const Json& item : page.items) {
            out.push_back(item["id"]);
        }
        return out;
    }

    std::int64_t createRoot() {
        commit({Json{{"type", "conversation"}, {"value", Json{{"id", kRoot}}}}});
        return kRoot;
    }

    Json conversation(std::int64_t id) const {
        return Json{{"type", "conversation"}, {"value", Json{{"id", id}}}};
    }

    Json entryWrite(const Json& record) const {
        return Json{{"type", "entry"}, {"value", record}};
    }

    Json entry(std::int64_t id, std::int64_t conversationId, const std::string& kind = "message",
               const Json& extra = Json::object()) const {
        Json record = {{"id", id}, {"conversationId", conversationId}, {"kind", kind}};
        for (const auto& item : extra.items()) {
            record[item.key()] = item.value();
        }
        return record;
    }

    Json pendingTask(std::int64_t id, std::int64_t conversationId, const std::string& phase = "ready") const {
        return Json{{"id", id},
                    {"conversationId", conversationId},
                    {"kind", "test.task"},
                    {"version", 1},
                    {"input", Json{{"value", id}}},
                    {"state", Json{{"status", "pending"}, {"checkpoint", Json{{"phase", phase}}}}},
                    {"background", false},
                    {"abortRequested", false}};
    }

    Json taskWrite(const Json& record) const {
        return Json{{"type", "task"}, {"value", record}};
    }

    Json submissionWrite(const Json& record) const {
        return Json{{"type", "submission"}, {"value", record}};
    }

    Json inputSubmission(std::int64_t id, std::int64_t conversationId, const std::string& request,
                         const std::string& type = "input", const std::string& status = "queued") const {
        Json record = {{"id", id}, {"conversationId", conversationId}};
        if (!request.empty()) {
            record["requestId"] = request;
        }
        record["type"] = type;
        record["status"] = status;
        return record;
    }

    Json sessionScope() const {
        return Json{{"kind", "session"}};
    }

    Json conversationScope(std::int64_t id) const {
        return Json{{"kind", "conversation"}, {"conversationId", id}};
    }

    Json taskScope(std::int64_t id) const {
        return Json{{"kind", "task"}, {"taskId", id}};
    }

    Json documentRecord(std::int64_t id, const std::string& kind, const Json& scope, const std::string& history = "",
                        const std::string& fork = "", const std::string& key = "") const {
        Json record = {{"id", id}, {"kind", kind}, {"scope", scope}};
        if (!key.empty()) {
            record["key"] = key;
        }
        if (!history.empty()) {
            record["history"] = history;
            record["fork"] = fork;
        }
        return record;
    }

    Json createDocument(const Json& record, std::int64_t version, const Json& body) const {
        return Json{{"type", "document.create"},
                    {"record", record},
                    {"content", Json{{"kind", "base"}, {"version", version}, {"value", body}}}};
    }

    Json changeBase(std::int64_t id, std::int64_t version, const Json& body) const {
        return Json{{"type", "document.change"},
                    {"id", id},
                    {"content", Json{{"kind", "base"}, {"version", version}, {"value", body}}}};
    }

    Json changeDelta(std::int64_t id, std::int64_t version, const Json& ops) const {
        return Json{{"type", "document.change"},
                    {"id", id},
                    {"content", Json{{"kind", "delta"}, {"version", version}, {"ops", ops}}}};
    }

    Json retire(std::int64_t id) const {
        return Json{{"type", "document.retire"}, {"id", id}};
    }

    DocumentPoint at(std::int64_t seq) const {
        return DocumentPoint{false, seq};
    }

    Json documentValue(std::int64_t id, const DocumentPoint& point = DocumentPoint()) {
        const auto stored = value(m_storage->document(id, point));
        return stored ? stored->value : Json();
    }

    void reservesRoot() {
        same(value(m_storage->mintId()), 2);
        check(commit({conversation(kRoot)}) > 0, "first commit has a sequence");
        same(found(m_storage->conversation(kRoot)), Json{{"id", kRoot}});
        rejectsWith(m_storage->commit({conversation(kRoot)}), "ID 1 already belongs to conversation");
    }

    void mixedWritesAreAtomic() {
        const std::int64_t root = createRoot();
        const std::int64_t entryId = mint();
        const std::int64_t taskId = mint();
        const std::int64_t submissionId = mint();
        const Json task = pendingTask(taskId, root);
        Json input = inputSubmission(submissionId, root, "request-1", "input", "placed");
        input["entry"] = entryId;
        const Json userEntry = entry(entryId, root, "user", Json{{"data", Json{{"text", "hello"}}}});
        const std::int64_t initialSeq = commit({entryWrite(userEntry), taskWrite(task), submissionWrite(input)});
        const auto looked = value(m_storage->entry(entryId));
        check(looked.has_value(), "entry readable");
        if (looked) {
            same(looked->entry, userEntry);
            same(looked->commitSeq, initialSeq);
        }
        same(found(m_storage->task(taskId)), task);
        same(found(m_storage->submission(submissionId)), input);

        const std::int64_t transientEntry = mint();
        Json running = task;
        running["state"] = Json{{"status", "running"}, {"checkpoint", Json{{"phase", "effect"}}}};
        Json done = input;
        done["status"] = "done";
        done["answer"] = transientEntry;
        rejectsWith(m_storage->commit({taskWrite(running), submissionWrite(done),
                                       entryWrite(entry(transientEntry, root, "assistant")), conversation(root)}),
                    "ID 1 already belongs to conversation");
        same(found(m_storage->task(taskId)), task);
        same(found(m_storage->submission(submissionId)), input);
        check(!value(m_storage->entry(transientEntry)).has_value(), "rolled back entry is absent");
        check(commit({entryWrite(entry(mint(), root, "after-rollback"))}) > initialSeq, "sequence keeps increasing");
    }

    void detachesRecords() {
        const std::int64_t root = createRoot();
        const std::int64_t entryId = mint();
        Json data = Json{{"nested", Json::array({1, 2})}};
        Json written = entry(entryId, root, "note", Json{{"data", data}});
        commit({entryWrite(written)});
        data["nested"].push_back(3);
        written["data"]["nested"].push_back(4);
        auto read = value(m_storage->entry(entryId));
        same(read->entry["data"], Json{{"nested", Json::array({1, 2})}});
        read->entry["data"]["nested"].push_back(9);
        same(value(m_storage->entry(entryId))->entry["data"], Json{{"nested", Json::array({1, 2})}});
    }

    void entriesOutOfOrder() {
        const std::int64_t root = createRoot();
        commit({entryWrite(entry(30, root)), entryWrite(entry(10, root)),
                entryWrite(entry(20, root, "marker", Json{{"head", 10}}))});
        EntryQuery query;
        query.conversationId = root;
        same(ids(value(m_storage->scanEntries(query, 10, std::nullopt))), Json::array({30, 20, 10}));
        same(found(m_storage->findLatestHeadMarker(root, std::nullopt))["id"], 20);
    }

    void entryCursor() {
        const std::int64_t root = createRoot();
        const std::int64_t oldest = mint();
        const std::int64_t middle = mint();
        const std::int64_t newest = mint();
        commit({entryWrite(entry(oldest, root)), entryWrite(entry(middle, root)), entryWrite(entry(newest, root))});
        EntryQuery query;
        query.conversationId = root;
        const auto first = value(m_storage->scanEntries(query, 2, std::nullopt));
        same(ids(first), Json::array({newest, middle}));
        commit({entryWrite(entry(mint(), root))});
        const auto second = value(m_storage->scanEntries(query, 2, first.next));
        same(ids(second), Json::array({oldest}));
        check(!second.next.has_value(), "last page has no cursor");
    }

    void conversationPages() {
        const std::int64_t root = createRoot();
        const std::int64_t second = mint();
        const std::int64_t third = mint();
        commit({conversation(third), conversation(second)});
        const auto first = value(m_storage->scanConversations(ConversationQuery{}, 2, std::nullopt));
        same(ids(first), Json::array({root, second}));
        check(first.next.has_value(), "first page has a cursor");
        const Json roundTripped = first.next ? Json::parse(first.next->dump()) : Json();
        const auto next = value(m_storage->scanConversations(ConversationQuery{}, 2, roundTripped));
        same(ids(next), Json::array({third}));
        check(!next.next.has_value(), "last page has no cursor");
    }

    void conversationOwners() {
        const std::int64_t root = createRoot();
        const std::int64_t otherOwner = mint();
        const std::int64_t firstTask = mint();
        const std::int64_t secondTask = mint();
        const std::int64_t first = mint();
        const std::int64_t second = mint();
        const std::int64_t third = mint();
        auto owned = [&](std::int64_t id, std::int64_t owner, std::int64_t task) {
            return Json{{"type", "conversation"},
                        {"value", Json{{"id", id}, {"owner", Json{{"conversationId", owner}, {"taskId", task}}}}}};
        };
        commit({conversation(otherOwner), owned(first, root, firstTask), owned(second, root, secondTask),
                owned(third, otherOwner, firstTask)});
        ConversationQuery byOwner;
        byOwner.ownerConversationId = root;
        const auto page = value(m_storage->scanConversations(byOwner, 1, std::nullopt));
        same(ids(page), Json::array({first}));
        const auto next = value(m_storage->scanConversations(byOwner, 1, page.next));
        same(ids(next), Json::array({second}));
        check(!next.next.has_value(), "last owner page has no cursor");
        ConversationQuery byTask;
        byTask.ownerTaskId = firstTask;
        same(ids(value(m_storage->scanConversations(byTask, 10, std::nullopt))), Json::array({first, third}));
        ConversationQuery both;
        both.ownerConversationId = root;
        both.ownerTaskId = firstTask;
        same(ids(value(m_storage->scanConversations(both, 10, std::nullopt))), Json::array({first}));
    }

    void forkHistory() {
        const std::int64_t root = createRoot();
        const std::int64_t rootFirst = mint();
        const std::int64_t rootForkPoint = mint();
        const std::int64_t rootExcludedSameCommit = mint();
        const std::int64_t rootEntriesSeq =
            commit({entryWrite(entry(rootFirst, root)),
                    entryWrite(entry(rootForkPoint, root, "marker", Json{{"head", rootFirst}})),
                    entryWrite(entry(rootExcludedSameCommit, root))});
        const std::int64_t child = mint();
        auto forked = [&](std::int64_t id, std::int64_t parent, std::int64_t atEntry) {
            return Json{{"type", "conversation"},
                        {"value", Json{{"id", id}, {"parent", Json{{"conversationId", parent}, {"at", atEntry}}}}}};
        };
        commit({forked(child, root, rootForkPoint)});
        const std::int64_t childForkPoint = mint();
        const std::int64_t childExcluded = mint();
        commit({entryWrite(entry(childForkPoint, child, "note")), entryWrite(entry(childExcluded, child))});
        const std::int64_t rootExcludedLater = mint();
        commit({entryWrite(entry(rootExcludedLater, root))});
        const std::int64_t grandchild = mint();
        commit({forked(grandchild, child, childForkPoint)});
        const std::int64_t grandchildHead = mint();
        const std::int64_t grandchildTail = mint();
        const std::int64_t grandchildSeq =
            commit({entryWrite(entry(grandchildHead, grandchild, "marker", Json{{"head", grandchildHead}})),
                    entryWrite(entry(grandchildTail, grandchild))});
        const std::int64_t childExcludedLater = mint();
        commit({entryWrite(entry(childExcludedLater, child))});

        EntryQuery query;
        query.conversationId = grandchild;
        const auto first = value(m_storage->scanEntries(query, 2, std::nullopt));
        same(ids(first), Json::array({grandchildTail, grandchildHead}));
        const auto second = value(m_storage->scanEntries(query, 2, first.next));
        same(ids(second), Json::array({childForkPoint, rootForkPoint}));
        const auto third = value(m_storage->scanEntries(query, 2, second.next));
        same(ids(third), Json::array({rootFirst}));
        check(!third.next.has_value(), "history ends");

        const Json current = found(m_storage->findLatestHeadMarker(grandchild, std::nullopt));
        same(current["id"], grandchildHead);
        same(current["head"], grandchildHead);
        const Json historical = found(m_storage->findLatestHeadMarker(grandchild, childForkPoint));
        same(historical["id"], rootForkPoint);
        same(historical["head"], rootFirst);
        same(found(m_storage->findLatestHeadMarker(grandchild, rootFirst)), Json());

        EntryQuery active = query;
        active.minEntryId = grandchildHead;
        const auto activeFirst = value(m_storage->scanEntries(active, 1, std::nullopt));
        same(ids(activeFirst), Json::array({grandchildTail}));
        check(activeFirst.next.has_value(), "active scan has a cursor");
        const auto activeSecond = value(m_storage->scanEntries(active, 1, activeFirst.next));
        same(ids(activeSecond), Json::array({grandchildHead}));
        check(!activeSecond.next.has_value(), "active scan ends");

        EntryQuery bounded = query;
        bounded.minEntryId = rootFirst;
        bounded.maxEntryId = childForkPoint;
        same(ids(value(m_storage->scanEntries(bounded, 10, std::nullopt))),
             Json::array({childForkPoint, rootForkPoint, rootFirst}));

        const auto globalFirst = value(m_storage->entry(rootFirst));
        same(globalFirst->entry, entry(rootFirst, root));
        same(globalFirst->commitSeq, rootEntriesSeq);
        same(value(m_storage->entry(grandchildHead))->commitSeq, grandchildSeq);
        check(!value(m_storage->entry(999999)).has_value(), "unknown entry");

        same(value(m_storage->visibleEntry(grandchild, rootFirst))->entry, entry(rootFirst, root));
        same(value(m_storage->visibleEntry(grandchild, childForkPoint))->entry["conversationId"], child);
        same(value(m_storage->visibleEntry(grandchild, grandchildTail))->commitSeq, grandchildSeq);
        for (const std::int64_t hidden : {rootExcludedSameCommit, rootExcludedLater, childExcluded, childExcludedLater, std::int64_t{999999}}) {
            check(!value(m_storage->visibleEntry(grandchild, hidden)).has_value(), "entry " + std::to_string(hidden) + " is hidden");
        }
        check(!value(m_storage->visibleEntry(root, grandchildHead)).has_value(), "descendant entries are not visible upward");
        rejectsWith(m_storage->visibleEntry(999999, rootFirst), "Unknown conversation");
        EntryQuery unknown;
        unknown.conversationId = 999999;
        rejectsWith(m_storage->scanEntries(unknown, 10, std::nullopt), "Unknown conversation");
    }

    void taskRecords() {
        const std::int64_t root = createRoot();
        const std::int64_t firstId = mint();
        const std::int64_t secondId = mint();
        const std::int64_t thirdId = mint();
        Json first = pendingTask(firstId, root);
        first["memos"] = Json{{"winner", "first"}};
        Json second = pendingTask(secondId, root);
        second["background"] = true;
        Json third = pendingTask(thirdId, root);
        third["abortRequested"] = true;
        commit({taskWrite(first), taskWrite(second), taskWrite(third)});

        Json running = first;
        running["state"] = Json{{"status", "running"}, {"checkpoint", Json{{"phase", "effect"}, {"attempt", 1}}}};
        running["abortRequested"] = true;
        commit({taskWrite(running)});
        same(found(m_storage->task(firstId)), running);
        const Json terminal = Json{{"id", firstId},
                                   {"conversationId", root},
                                   {"kind", first["kind"]},
                                   {"version", first["version"]},
                                   {"input", first["input"]},
                                   {"state", Json{{"status", "terminal"},
                                                  {"outcome", Json{{"status", "completed"}, {"result", Json{{"entryId", 99}}}}}}},
                                   {"background", false},
                                   {"abortRequested", true}};
        commit({taskWrite(terminal)});
        same(found(m_storage->task(firstId)), terminal);

        TaskQuery pending;
        pending.status = "pending";
        const auto page = value(m_storage->scanTasks(pending, 1, std::nullopt));
        same(ids(page), Json::array({secondId}));
        check(page.next.has_value(), "pending page has a cursor");
        same(ids(value(m_storage->scanTasks(pending, 1, page.next))), Json::array({thirdId}));
        TaskQuery aborted;
        aborted.status = "terminal";
        aborted.abortRequested = true;
        same(Json(value(m_storage->scanTasks(aborted, 10, std::nullopt)).items), Json::array({terminal}));
        TaskQuery background;
        background.background = true;
        same(ids(value(m_storage->scanTasks(background, 10, std::nullopt))), Json::array({secondId}));
    }

    void taskOwners() {
        const std::int64_t root = createRoot();
        const std::int64_t ownerId = mint();
        const std::int64_t waitingId = mint();
        const std::int64_t completingId = mint();
        const Json owner = pendingTask(ownerId, root);
        Json waiting = pendingTask(waitingId, root);
        waiting["owner"] = ownerId;
        waiting["state"] = Json{{"status", "waiting"},
                                {"checkpoint", Json{{"phase", "next"}},
                                 },
                                {"on", Json::array({ownerId})},
                                {"policy", "allSettled"}};
        waiting["memos"] = Json{{"kept", true}};
        Json completing = pendingTask(completingId, root);
        completing.erase("state");
        completing["owner"] = ownerId;
        completing["state"] = Json{{"status", "completing"},
                                   {"outcome", Json{{"status", "failed"}, {"error", Json{{"message", "held"}}}}}};
        commit({taskWrite(owner), taskWrite(waiting), taskWrite(completing)});
        same(found(m_storage->task(waitingId)), waiting);
        same(found(m_storage->task(completingId)), completing);
        auto scan = [&](const std::string& status) {
            TaskQuery query;
            query.status = status;
            return value(m_storage->scanTasks(query, 10, std::nullopt));
        };
        same(Json(scan("waiting").items), Json::array({waiting}));
        same(Json(scan("completing").items), Json::array({completing}));
        same(ids(scan("pending")), Json::array({ownerId}));
        Json terminal = completing;
        terminal["state"]["status"] = "terminal";
        commit({taskWrite(terminal)});
        check(scan("completing").items.empty(), "no completing tasks remain");
        same(Json(scan("terminal").items), Json::array({terminal}));
    }

    void requestIds() {
        const std::int64_t root = createRoot();
        const std::int64_t secondConversation = mint();
        commit({conversation(secondConversation)});
        const std::int64_t firstId = mint();
        const std::int64_t secondId = mint();
        const std::int64_t otherId = mint();
        const Json first = inputSubmission(firstId, root, "same");
        const Json second = inputSubmission(secondId, root, "other");
        const Json other = inputSubmission(otherId, secondConversation, "same");
        commit({submissionWrite(first), submissionWrite(second), submissionWrite(other)});
        same(found(m_storage->submissionByRequest(root, "same")), first);
        same(found(m_storage->submissionByRequest(secondConversation, "same")), other);

        Json placed = second;
        placed["status"] = "placed";
        placed["entry"] = mint();
        commit({submissionWrite(placed)});
        same(found(m_storage->submission(secondId)), placed);
        same(found(m_storage->submissionByRequest(root, "other")), placed);

        auto scanIds = [&](const SubmissionQuery& query) {
            Json found = Json::array();
            std::optional<Json> cursor;
            do {
                const auto page = value(m_storage->scanSubmissions(query, 1, cursor));
                for (const Json& item : page.items) {
                    found.push_back(item["id"]);
                }
                cursor = page.next;
            } while (cursor);
            return found;
        };
        same(scanIds(SubmissionQuery{}), Json::array({firstId, secondId, otherId}));
        SubmissionQuery rootOnly;
        rootOnly.conversationId = root;
        same(scanIds(rootOnly), Json::array({firstId, secondId}));
        SubmissionQuery queued;
        queued.status = "queued";
        same(scanIds(queued), Json::array({firstId, otherId}));
        SubmissionQuery placedQuery;
        placedQuery.status = "placed";
        same(scanIds(placedQuery), Json::array({secondId}));
        SubmissionQuery queuedInSecond;
        queuedInSecond.conversationId = secondConversation;
        queuedInSecond.status = "queued";
        same(scanIds(queuedInSecond), Json::array({otherId}));
        queuedInSecond.status = "placed";
        same(scanIds(queuedInSecond), Json::array());
        same(Json(value(m_storage->scanSubmissions(placedQuery, 10, std::nullopt)).items), Json::array({placed}));
    }

    void passiveWrites() {
        const std::int64_t root = createRoot();
        const std::int64_t doneId = mint();
        const std::int64_t failedId = mint();
        const Json queuedDone = inputSubmission(doneId, root, "passive-done", "write");
        const Json queuedFailed = inputSubmission(failedId, root, "passive-failed", "write");
        commit({submissionWrite(queuedDone), submissionWrite(queuedFailed)});
        Json done = queuedDone;
        done["status"] = "done";
        done["entry"] = mint();
        Json unanswered = queuedFailed;
        unanswered["status"] = "unanswered";
        unanswered["reason"] = "closed";
        unanswered["detail"] = Json{{"retryable", false}};
        commit({submissionWrite(done), submissionWrite(unanswered)});
        same(found(m_storage->submission(doneId)), done);
        same(found(m_storage->submissionByRequest(root, "passive-done")), done);
        same(found(m_storage->submission(failedId)), unanswered);
        same(found(m_storage->submissionByRequest(root, "passive-failed")), unanswered);
    }

    void rewindableDocuments() {
        const std::int64_t root = createRoot();
        const std::int64_t firstId = mint();
        const Json firstRecord = documentRecord(firstId, "conversation.notes", conversationScope(root), "rewindable", "asOf");
        const std::int64_t createdAt =
            commit({createDocument(firstRecord, 1, Json{{"items", Json::array({"a"})}, {"nested", Json{{"count", 1}}}})});
        const std::int64_t changedAt = commit(
            {changeDelta(firstId, 1, Json::array({Json::array({"p", Json::array({"items"}), 1, 0, Json::array({"b"})}),
                                                  Json::array({"s", Json::array({"nested", "count"}), 2})}))});
        const auto initial = value(m_storage->document(firstId, at(createdAt)));
        same(initial->value, Json{{"items", Json::array({"a"})}, {"nested", Json{{"count", 1}}}});
        same(initial->deltasSinceBase, 0);
        const auto changed = value(m_storage->document(firstId, at(changedAt)));
        same(changed->value, Json{{"items", Json::array({"a", "b"})}, {"nested", Json{{"count", 2}}}});
        same(changed->deltasSinceBase, 1);

        const std::int64_t checkpointAt =
            commit({changeBase(firstId, 2, Json{{"items", Json::array({"checkpoint"})}, {"nested", Json{{"count", 3}}}})});
        const std::int64_t replacedAt = commit(
            {changeDelta(firstId, 2,
                         Json::array({Json::array({"r", Json{{"items", Json::array({"replacement"})}, {"nested", Json{{"count", 4}}}}})}))});
        same(value(m_storage->document(firstId, at(changedAt)))->version, 1);
        const auto checkpoint = value(m_storage->document(firstId, at(checkpointAt)));
        same(checkpoint->version, 2);
        same(checkpoint->deltasSinceBase, 0);
        const auto replaced = value(m_storage->document(firstId, at(replacedAt)));
        same(replaced->value, Json{{"items", Json::array({"replacement"})}, {"nested", Json{{"count", 4}}}});
        same(replaced->deltasSinceBase, 1);
        same(value(m_storage->document(firstId, DocumentPoint()))->deltasSinceBase, 1);

        const std::int64_t secondId = mint();
        Json secondRecord = firstRecord;
        secondRecord["id"] = secondId;
        const std::int64_t retiredAt =
            commit({createDocument(secondRecord, 1, Json{{"items", Json::array({"new"})}}), retire(firstId),
                    changeDelta(firstId, 2, Json::array({Json::array({"s", Json::array({"retiring"}), true})}))});
        DocumentAddress address{"conversation.notes", conversationScope(root), std::nullopt};
        same(found(m_storage->findDocument(address, at(changedAt)))["id"], firstId);
        const Json replacement = found(m_storage->findDocument(address, at(retiredAt)));
        same(replacement["id"], secondId);
        same(replacement["createdAt"], retiredAt);
        DocumentQuery query{conversationScope(root), at(changedAt), std::nullopt};
        same(ids(value(m_storage->scanDocuments(query, 10, std::nullopt))), Json::array({firstId}));
        query.at = at(retiredAt);
        same(ids(value(m_storage->scanDocuments(query, 10, std::nullopt))), Json::array({secondId}));
        check(!value(m_storage->document(firstId, at(retiredAt))).has_value(), "retired incarnation is gone at its retirement");
        same(documentValue(secondId), Json{{"items", Json::array({"new"})}});
    }

    void longTails() {
        const std::int64_t root = createRoot();
        const std::int64_t id = mint();
        const Json record = documentRecord(id, "conversation.long-tail", conversationScope(root), "rewindable", "asOf");
        auto rows = [](int base, const std::string& prefix) {
            Json list = Json::array();
            for (int value = 0; value < 512; ++value) {
                list.push_back(Json{{"value", base + value}, {"stable", prefix + std::to_string(value)}});
            }
            return list;
        };
        Json initial = Json{{"revision", 0}, {"rows", rows(0, "row-")}};
        const std::int64_t createdAt = commit({createDocument(record, 1, initial)});
        Json before = initial;
        std::int64_t beforeAt = createdAt;
        for (int revision = 1; revision <= 24; ++revision) {
            const int index = (revision * 17) % 512;
            before["rows"][index]["value"] = -revision;
            before["revision"] = revision;
            beforeAt = commit({changeDelta(id, 1,
                                           Json::array({Json::array({"s", Json::array({"rows", index, "value"}), -revision}),
                                                        Json::array({"s", Json::array({"revision"}), revision})}))});
        }
        const Json replacement = Json{{"revision", 100}, {"rows", rows(10000, "new-")}};
        const std::int64_t replacedAt = commit({changeDelta(id, 1, Json::array({Json::array({"r", replacement})}))});
        Json current = replacement;
        for (int revision = 101; revision <= 124; ++revision) {
            const int index = (revision * 19) % 512;
            current["rows"][index]["value"] = -revision;
            current["revision"] = revision;
            commit({changeDelta(id, 1,
                                Json::array({Json::array({"s", Json::array({"rows", index, "value"}), -revision}),
                                             Json::array({"s", Json::array({"revision"}), revision})}))});
        }
        same(documentValue(id, at(createdAt)), initial);
        same(documentValue(id, at(beforeAt)), before);
        same(documentValue(id, at(replacedAt)), replacement);
        auto read = value(m_storage->document(id, DocumentPoint()));
        same(read->value, current);
        read->value["rows"][0]["value"] = -1000;
        same(documentValue(id), current);
    }

    void documentCopies() {
        const std::int64_t root = createRoot();
        const std::int64_t child = mint();
        const std::int64_t secondChild = mint();
        commit({conversation(child), conversation(secondChild)});
        const std::int64_t sourceId = mint();
        const Json sourceRecord = documentRecord(sourceId, "copy.source", conversationScope(root), "rewindable", "asOf");
        const std::int64_t createdAt = commit(
            {createDocument(sourceRecord, 2,
                            Json{{"count", 1}, {"rows", Json::array({Json{{"value", "base"}}})}})});
        commit({changeDelta(sourceId, 2,
                            Json::array({Json::array({"s", Json::array({"count"}), 2}),
                                         Json::array({"p", Json::array({"rows"}), 1, 0, Json::array({Json{{"value", "current"}}})})}))});
        const std::int64_t historicalCopy = mint();
        const std::int64_t currentCopy = mint();
        const std::int64_t retiredCopy = mint();
        auto childRecord = [&](std::int64_t id, std::int64_t conversationId) {
            return documentRecord(id, "copy.source", conversationScope(conversationId), "rewindable", "asOf");
        };
        auto copy = [&](const Json& record, const Json& sourceAt) {
            return Json{{"type", "document.copy"}, {"record", record}, {"source", Json{{"id", sourceId}, {"at", sourceAt}}}};
        };
        commit({copy(childRecord(historicalCopy, child), createdAt), copy(childRecord(currentCopy, secondChild), "current"),
                copy(childRecord(retiredCopy, root), "current"), retire(retiredCopy)});
        const auto historical = value(m_storage->document(historicalCopy, DocumentPoint()));
        same(historical->version, 2);
        same(historical->value, Json{{"count", 1}, {"rows", Json::array({Json{{"value", "base"}}})}});
        const Json currentValue = Json{{"count", 2}, {"rows", Json::array({Json{{"value", "base"}}, Json{{"value", "current"}}})}};
        same(documentValue(currentCopy), currentValue);
        check(!value(m_storage->document(retiredCopy, DocumentPoint())).has_value(), "retired copy is gone");

        commit({changeBase(sourceId, 2, Json{{"count", 99}, {"rows", Json::array()}}), retire(sourceId)});
        same(documentValue(currentCopy), currentValue);

        const std::int64_t latestSource = mint();
        const std::int64_t latestCopy = mint();
        const Json latestRecord = documentRecord(latestSource, "copy.latest", conversationScope(root), "latest", "current");
        commit({createDocument(latestRecord, 4, Json{{"retained", "copy"}})});
        Json latestCopyRecord = latestRecord;
        latestCopyRecord["id"] = latestCopy;
        latestCopyRecord["scope"] = conversationScope(child);
        commit({Json{{"type", "document.copy"},
                     {"record", latestCopyRecord},
                     {"source", Json{{"id", latestSource}, {"at", "current"}}}}});
        commit({changeBase(latestSource, 4, Json{{"retained", "source-only"}}), retire(latestSource)});
        const auto latest = value(m_storage->document(latestCopy, DocumentPoint()));
        same(latest->version, 4);
        same(latest->value, Json{{"retained", "copy"}});

        const std::int64_t conflict = mint();
        auto conflicting = m_storage->commit({Json{{"type", "document.copy"},
                                                   {"record", childRecord(conflict, child)},
                                                   {"source", Json{{"id", currentCopy}, {"at", "current"}}}},
                                              retire(currentCopy)});
        check(!conflicting && conflicting.error().code == "storage_rejected", "copy of a document changed in the batch is rejected");
        check(!value(m_storage->document(conflict, DocumentPoint())).has_value(), "rejected copy left nothing");
        same(documentValue(currentCopy), currentValue);

        const std::int64_t mismatch = mint();
        Json mismatched = childRecord(mismatch, child);
        mismatched["kind"] = "copy.mismatch";
        auto mismatching = m_storage->commit({Json{{"type", "document.copy"},
                                                   {"record", mismatched},
                                                   {"source", Json{{"id", currentCopy}, {"at", "current"}}}}});
        check(!mismatching && mismatching.error().code == "storage_rejected", "copy into another kind is rejected");
        check(!value(m_storage->document(mismatch, DocumentPoint())).has_value(), "mismatched copy left nothing");
    }

    void versionTransitions() {
        createRoot();
        const std::int64_t id = mint();
        const Json record = documentRecord(id, "session.settings", sessionScope());
        commit({createDocument(record, 1, Json{{"count", 1}})});
        commit({changeDelta(id, 1, Json::array({Json::array({"s", Json::array({"count"}), 2})}))});
        const std::int64_t migratedAt = commit({changeBase(id, 2, Json{{"count", 3}})});
        const auto current = value(m_storage->document(id, DocumentPoint()));
        same(current->version, 2);
        same(current->value, Json{{"count", 3}});
        rejectsWith(m_storage->document(id, at(migratedAt)), "does not retain historical content");
        rejectsWith(m_storage->commit({changeDelta(id, 1, Json::array({Json::array({"s", Json::array({"count"}), 4})}))}),
                    "version transition requires a base");
        same(documentValue(id), Json{{"count", 3}});
        commit({retire(id)});
        check(!value(m_storage->document(id, DocumentPoint())).has_value(), "retired document is gone");
    }

    void addressesAndScopes() {
        const std::int64_t root = createRoot();
        const std::int64_t firstId = mint();
        const std::int64_t secondId = mint();
        const std::int64_t conversationDoc = mint();
        const std::int64_t taskId = mint();
        const std::int64_t taskSingleton = mint();
        const std::int64_t taskFamily = mint();
        const std::int64_t taskOtherKind = mint();
        const std::int64_t createdAt = commit(
            {taskWrite(pendingTask(taskId, root)),
             createDocument(documentRecord(firstId, "cache", sessionScope(), "", "", "__proto__"), 1, Json{{"owner", "first"}}),
             createDocument(documentRecord(secondId, "cache", sessionScope(), "", "", "constructor"), 1, Json{{"owner", "second"}}),
             createDocument(documentRecord(conversationDoc, "cache", conversationScope(root), "latest", "current", "__proto__"), 1,
                            Json{{"owner", "conversation"}}),
             createDocument(documentRecord(taskSingleton, "task.cache", taskScope(taskId)), 1, Json{{"owner", "singleton"}}),
             createDocument(documentRecord(taskFamily, "task.cache", taskScope(taskId), "", "", "member"), 1, Json{{"owner", "family"}}),
             createDocument(documentRecord(taskOtherKind, "task.other", taskScope(taskId)), 1, Json{{"owner", "other"}})});
        same(found(m_storage->findDocument(DocumentAddress{"cache", sessionScope(), std::string("__proto__")}, DocumentPoint()))["id"], firstId);
        DocumentQuery session{sessionScope(), DocumentPoint(), std::nullopt};
        const auto first = value(m_storage->scanDocuments(session, 1, std::nullopt));
        same(first.items.size(), 1);
        const auto second = value(m_storage->scanDocuments(session, 1, first.next));
        same(Json::array({first.items.at(0)["id"], second.items.at(0)["id"]}), Json::array({firstId, secondId}));
        DocumentQuery conversationQuery{conversationScope(root), DocumentPoint(), std::nullopt};
        same(ids(value(m_storage->scanDocuments(conversationQuery, 10, std::nullopt))), Json::array({conversationDoc}));
        same(found(m_storage->findDocument(DocumentAddress{"task.cache", taskScope(taskId), std::nullopt}, DocumentPoint()))["id"], taskSingleton);
        same(found(m_storage->findDocument(DocumentAddress{"task.cache", taskScope(taskId), std::string("member")}, DocumentPoint()))["id"], taskFamily);
        DocumentQuery taskQuery{taskScope(taskId), DocumentPoint(), std::string("task.cache")};
        same(ids(value(m_storage->scanDocuments(taskQuery, 10, std::nullopt))), Json::array({taskSingleton, taskFamily}));
        rejectsWith(m_storage->document(taskSingleton, at(createdAt)), "does not retain historical content");
    }

    void lifecycleFailures() {
        const std::int64_t root = createRoot();
        const std::int64_t firstId = mint();
        const std::int64_t secondId = mint();
        const Json record = documentRecord(firstId, "singleton", sessionScope());
        commit({createDocument(record, 1, Json{{"value", 1}})});
        Json second = record;
        second["id"] = secondId;
        rejectsWith(m_storage->commit({createDocument(second, 1, Json{{"value", 2}}),
                                       changeDelta(firstId, 1, Json::array())}),
                    "already has a current incarnation");
        same(documentValue(firstId), Json{{"value", 1}});
        check(!value(m_storage->document(secondId, DocumentPoint())).has_value(), "rejected create left nothing");

        const std::int64_t emptyId = mint();
        const Json emptyRecord = documentRecord(emptyId, "singleton", conversationScope(root), "rewindable", "initial", "empty");
        const std::int64_t emptyAt = commit({createDocument(emptyRecord, 1, Json::object()), retire(emptyId)});
        check(!value(m_storage->document(emptyId, DocumentPoint())).has_value(), "created-and-retired is not current");
        check(!value(m_storage->document(emptyId, at(emptyAt))).has_value(), "created-and-retired has an empty lifetime");
        check(!found(m_storage->findDocument(DocumentAddress{"singleton", conversationScope(root), std::string("empty")}, at(emptyAt))).is_object(),
              "no incarnation at the address");
    }

    void rollsBackIndexes() {
        const std::int64_t root = createRoot();
        const std::int64_t taskId = mint();
        const std::int64_t submissionId = mint();
        const std::int64_t documentId = mint();
        const Json task = pendingTask(taskId, root);
        const Json submission = inputSubmission(submissionId, root, "atomic");
        const Json record = documentRecord(documentId, "atomic", sessionScope());
        const std::int64_t baseline = commit({taskWrite(task), submissionWrite(submission), createDocument(record, 1, Json{{"count", 1}})});

        const std::int64_t entryId = mint();
        const std::int64_t conflicting = mint();
        Json running = task;
        running["state"] = Json{{"status", "running"}, {"checkpoint", Json{{"phase", "effect"}}}};
        Json failed = submission;
        failed["status"] = "unanswered";
        failed["reason"] = "failed";
        Json duplicate = record;
        duplicate["id"] = conflicting;
        rejectsWith(m_storage->commit({taskWrite(running), submissionWrite(failed),
                                       entryWrite(entry(entryId, root, "transient")),
                                       createDocument(duplicate, 1, Json{{"count", 2}})}),
                    "already has a current incarnation");
        same(found(m_storage->task(taskId)), task);
        TaskQuery pending;
        pending.status = "pending";
        same(Json(value(m_storage->scanTasks(pending, 10, std::nullopt)).items), Json::array({task}));
        same(found(m_storage->submissionByRequest(root, "atomic")), submission);
        check(!value(m_storage->entry(entryId)).has_value(), "transient entry rolled back");
        check(!value(m_storage->document(conflicting, DocumentPoint())).has_value(), "conflicting document rolled back");
        same(found(m_storage->findDocument(DocumentAddress{"atomic", sessionScope(), std::nullopt}, DocumentPoint()))["id"], documentId);
        check(commit({changeDelta(documentId, 1, Json::array({Json::array({"s", Json::array({"count"}), 3})}))}) > baseline,
              "sequence continues after rollback");
    }

    void stringIdentities() {
        const std::int64_t root = createRoot();
        const std::string first = "a\"b|c";
        const std::string second = "a\"b|d";
        const std::int64_t firstTask = mint();
        const std::int64_t secondTask = mint();
        const std::int64_t firstSubmission = mint();
        const std::int64_t secondSubmission = mint();
        const std::int64_t firstKindDoc = mint();
        const std::int64_t secondKindDoc = mint();
        const std::int64_t firstKeyDoc = mint();
        const std::int64_t secondKeyDoc = mint();
        Json firstTaskRecord = pendingTask(firstTask, root);
        firstTaskRecord["kind"] = first;
        Json secondTaskRecord = pendingTask(secondTask, root);
        secondTaskRecord["kind"] = second;
        commit({taskWrite(firstTaskRecord), taskWrite(secondTaskRecord),
                submissionWrite(inputSubmission(firstSubmission, root, first)),
                submissionWrite(inputSubmission(secondSubmission, root, second)),
                createDocument(documentRecord(firstKindDoc, first, sessionScope()), 1, Json{{"identity", "first kind"}}),
                createDocument(documentRecord(secondKindDoc, second, sessionScope()), 1, Json{{"identity", "second kind"}}),
                createDocument(documentRecord(firstKeyDoc, "family", sessionScope(), "", "", first), 1, Json{{"identity", "first key"}}),
                createDocument(documentRecord(secondKeyDoc, "family", sessionScope(), "", "", second), 1, Json{{"identity", "second key"}})});
        TaskQuery byKind;
        byKind.kind = first;
        same(ids(value(m_storage->scanTasks(byKind, 10, std::nullopt))), Json::array({firstTask}));
        byKind.kind = second;
        same(ids(value(m_storage->scanTasks(byKind, 10, std::nullopt))), Json::array({secondTask}));
        same(found(m_storage->submissionByRequest(root, first))["id"], firstSubmission);
        same(found(m_storage->submissionByRequest(root, second))["id"], secondSubmission);
        same(found(m_storage->findDocument(DocumentAddress{first, sessionScope(), std::nullopt}, DocumentPoint()))["id"], firstKindDoc);
        same(found(m_storage->findDocument(DocumentAddress{second, sessionScope(), std::nullopt}, DocumentPoint()))["id"], secondKindDoc);
        same(found(m_storage->findDocument(DocumentAddress{"family", sessionScope(), first}, DocumentPoint()))["id"], firstKeyDoc);
        same(found(m_storage->findDocument(DocumentAddress{"family", sessionScope(), second}, DocumentPoint()))["id"], secondKeyDoc);
        DocumentQuery byDocumentKind{sessionScope(), DocumentPoint(), first};
        same(ids(value(m_storage->scanDocuments(byDocumentKind, 10, std::nullopt))), Json::array({firstKindDoc}));
    }

    void idNamespace() {
        const std::int64_t root = createRoot();
        commit({entryWrite(entry(100, root))});
        same(value(m_storage->mintId()), 101);
        rejectsWith(m_storage->commit({taskWrite(pendingTask(100, root))}), "ID 100 already belongs to entry");
        commit({entryWrite(entry(9007199254740991LL, root, "last-id"))});
        rejectsWith(m_storage->mintId(), "ID space is exhausted");
        rejectsWith(m_storage->mintId(), "ID space is exhausted");
    }

    void rejectsAfterClose() {
        createRoot();
        value(m_storage->close());
        rejectsWith(m_storage->conversation(kRoot), "closed");
        rejectsWith(m_storage->commit({}), "closed");
        rejectsWith(m_storage->mintId(), "closed");
    }

    std::string m_case;
    std::shared_ptr<IStorage> m_storage;
    std::vector<std::string> m_failures;
};
