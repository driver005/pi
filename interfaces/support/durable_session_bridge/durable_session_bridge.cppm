export module pi.support.durable_session_bridge;

import std;
export import pi.plugin.i_plugin_session_bridge;
export import pi.support.conversation;

/**
 * What plugin commands can do to a durable (`pi serve`) session: the operations of
 * PiHostApi.session_call that make sense over a conversation. `appendEntry` is a write submission
 * of a `pi.plugin-entry` entry (`{customType, data}`) answering the submission id;
 * `sendUserMessage` is an input submission (steering, or a follow-up with `deliverAs: "followUp"`,
 * while the conversation is busy); `abort`, `waitForIdle`, `isIdle`, `sessionManager.getEntries`
 * (oldest first, at most 1000), `getSessionId` and `getCwd` read or drive the conversation.
 * Everything else answers `{"error": ...}`: models, tools, settings and the tree live in the
 * session registry of a durable session, not in a handle plugins can reach.
 */
export class DurableSessionBridge : public IPluginSessionBridge {
public:
    DurableSessionBridge(std::shared_ptr<Conversation> conversation, std::string cwd)
        : m_conversation(std::move(conversation)), m_cwd(std::move(cwd)) {}

    Json call(const std::string& method, const Json& params, const AbortSignal* abort) override {
        if (method == "appendEntry") {
            return appendEntry(params);
        }
        if (method == "sendUserMessage") {
            return sendUserMessage(params);
        }
        if (method == "abort") {
            const auto aborted = m_conversation->abort({}, abort);
            return aborted ? Json{{"ok", true}} : error(aborted.error().message);
        }
        if (method == "waitForIdle") {
            const auto idle = m_conversation->waitForIdle(abort);
            return Json{{"ok", idle.has_value()}};
        }
        if (method == "isIdle") {
            AbortSignal immediate;
            immediate.abort();
            return Json{{"idle", m_conversation->waitForIdle(&immediate).has_value()}};
        }
        if (method == "sessionManager.getEntries") {
            return entries();
        }
        if (method == "sessionManager.getSessionId") {
            return Json{{"id", std::to_string(m_conversation->id())}};
        }
        if (method == "sessionManager.getCwd") {
            return Json{{"cwd", m_cwd}};
        }
        return error("\"" + method + "\" is not available in pi serve sessions");
    }

private:
    Json appendEntry(const Json& params) {
        if (!params.contains("customType") || !params["customType"].is_string()) {
            return error("appendEntry needs a customType");
        }
        SubmissionDraft draft;
        draft.type = "write";
        draft.entry = Json{
            {"kind", "pi.plugin-entry"},
            {"data", Json{{"customType", params["customType"]},
                          {"data", params.contains("data") ? params["data"] : Json(nullptr)}}}};
        const auto submitted = m_conversation->submit(draft);
        return submitted ? Json{{"id", std::to_string((*submitted)->id())}}
                         : error(submitted.error().message);
    }

    Json sendUserMessage(const Json& params) {
        if (!params.contains("text") || !params["text"].is_string()) {
            return error("sendUserMessage needs text");
        }
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = params["text"];
        draft.whenBusy =
            params.value("deliverAs", std::string()) == "followUp" ? "followUp" : "steer";
        const auto submitted = m_conversation->submit(draft);
        return submitted ? Json{{"ok", true}} : error(submitted.error().message);
    }

    Json entries() {
        const auto page = m_conversation->entries(std::nullopt, std::nullopt, 1000);
        if (!page) {
            return error(page.error().message);
        }
        Json out = Json::array();
        for (auto it = page->items.rbegin(); it != page->items.rend(); ++it) {
            out.push_back(*it);
        }
        return out;
    }

    Json error(const std::string& message) const {
        return Json{{"error", message}};
    }

    std::shared_ptr<Conversation> m_conversation;
    std::string m_cwd;
};
