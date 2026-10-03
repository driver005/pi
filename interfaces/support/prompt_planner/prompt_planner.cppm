module;

#include <cstdint>

export module pi.support.prompt_planner;

import std;
export import pi.support.abort_signal;
export import pi.support.entry_kinds;
export import pi.types.error;
export import pi.types.json;
export import pi.types.prompt_input;
export import pi.types.prompt_section;
export import pi.types.result;
export import pi.types.tool_registration;

/**
 * Plans the positional `pi.system` entries that keep a conversation's replayed system prompt sections and tool
 * loadout equal to what its agent wants (spec §8.3). Messages, tools and entries are JSON in the shapes of the
 * transcript. Port of packages/durable/src/harness/prompt.ts.
 */
export class PromptPlanner {
public:
    /** Section texts in order; a patch is applied in place. */
    using Sections = std::vector<std::pair<std::string, std::string>>;

    /** Sections in effect after replaying system messages in order: set in place, `null` deletes, re-adding appends. */
    Sections replaySections(const Json& messages) const {
        Sections shown;
        for (const Json& message : messages) {
            if (message.value("role", std::string()) != "system" || !message.contains("sections")) {
                continue;
            }
            for (const auto& entry : message.at("sections").items()) {
                auto existing = std::find_if(shown.begin(), shown.end(), [&](const auto& item) { return item.first == entry.key(); });
                if (entry.value().is_null()) {
                    if (existing != shown.end()) {
                        shown.erase(existing);
                    }
                } else if (existing != shown.end()) {
                    existing->second = entry.value().get<std::string>();
                } else {
                    shown.emplace_back(entry.key(), entry.value().get<std::string>());
                }
            }
        }
        return shown;
    }

    /** The tools in effect after replaying system messages: removals first, then additions (replacing in place). */
    std::vector<Json> currentTools(const Json& messages) const {
        std::vector<Json> tools;
        for (const Json& message : messages) {
            if (message.value("role", std::string()) != "system") {
                continue;
            }
            if (message.contains("toolsRemoved")) {
                for (const Json& removed : message.at("toolsRemoved")) {
                    std::erase_if(tools, [&](const Json& tool) { return tool.at("name") == removed.at("name"); });
                }
            }
            if (message.contains("toolsAdded")) {
                for (const Json& added : message.at("toolsAdded")) {
                    auto existing = std::find_if(tools.begin(), tools.end(), [&](const Json& tool) { return tool.at("name") == added.at("name"); });
                    if (existing != tools.end()) {
                        *existing = added;
                    } else {
                        tools.push_back(added);
                    }
                }
            }
        }
        return tools;
    }

    /** The declaration of a registered tool as it enters the transcript. */
    Json declaration(const ToolRegistration& tool) const {
        return Json::object({{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}});
    }

    /**
     * Renders the agent's sections in order. Nothing omits a section; tagged text is wrapped as `<key>\n...\n</key>`. A
     * section that fails keeps its shown text, if any, and is reported; failures after `cancel` aborted propagate.
     */
    Result<Sections> render(const std::vector<PromptSection>& sections, const PromptInput& input, const Sections& shown,
                            const std::function<void(const Error&)>& report, const AbortSignal* cancel) const {
        Sections desired;
        for (const PromptSection& section : sections) {
            Result<std::optional<std::string>> text = section.render ? section.render(input) : Result<std::optional<std::string>>(std::nullopt);
            if (!text) {
                if (cancel != nullptr && cancel->aborted()) {
                    return std::unexpected(text.error());
                }
                report(text.error());
                auto kept = std::find_if(shown.begin(), shown.end(), [&](const auto& item) { return item.first == section.key; });
                if (kept != shown.end()) {
                    desired.push_back(*kept);
                }
                continue;
            }
            if (!*text) {
                continue;
            }
            desired.emplace_back(section.key, section.tag ? "<" + section.key + ">\n" + **text + "\n</" + section.key + ">" : **text);
        }
        return desired;
    }

    /**
     * Plans the `pi.system` entry drafts that make the replayed sections and tools of `view` equal `desired` and
     * `tools` in values and order.
     *
     * - A head marker with no later `pi.system` entry in context: one complete baseline that omits every retained
     *   earlier `pi.system` entry, written even when it restates the replayed values.
     * - Otherwise, when a minimal section patch would leave a different order: remove every shown section, then re-add
     *   every desired section in order.
     * - Otherwise the minimal patch of changed values and `null` removals, or nothing.
     *
     * Tool changes ride on the last planned entry, or on one entry of their own.
     */
    std::vector<Json> plan(const Json& view, const Sections& desired, const std::vector<ToolRegistration>& tools,
                           std::int64_t timestamp) const {
        std::vector<Json> wanted;
        for (const ToolRegistration& tool : tools) {
            wanted.push_back(declaration(tool));
        }
        const Json& head = view.at("head");
        if (!head.is_null() && !hasLaterSystemEntry(view.at("entries"), head.at("id").get<std::int64_t>())) {
            return {baseline(view, desired, wanted, timestamp)};
        }
        const std::vector<Json> patches = planSections(replaySections(view.at("messages")), desired);
        const ToolChanges changes = planTools(currentTools(view.at("messages")), wanted);
        if (changes.removed.empty() && changes.added.empty()) {
            std::vector<Json> drafts;
            for (const Json& patch : patches) {
                drafts.push_back(systemEntry(&patch, nullptr, timestamp));
            }
            return drafts;
        }
        if (patches.empty()) {
            return {systemEntry(nullptr, &changes, timestamp)};
        }
        std::vector<Json> drafts;
        for (std::size_t i = 0; i < patches.size(); ++i) {
            drafts.push_back(systemEntry(&patches[i], i + 1 == patches.size() ? &changes : nullptr, timestamp));
        }
        return drafts;
    }

private:
    struct ToolChanges {
        std::vector<Json> removed;
        std::vector<Json> added;
    };

    bool hasLaterSystemEntry(const Json& entries, std::int64_t headId) const {
        for (const Json& entry : entries) {
            if (m_kinds.is(entry, m_kinds.system()) && entry.at("id").get<std::int64_t>() > headId) {
                return true;
            }
        }
        return false;
    }

    Json baseline(const Json& view, const Sections& desired, const std::vector<Json>& wanted, std::int64_t timestamp) const {
        Json sections = Json::object();
        for (const auto& item : desired) {
            sections[item.first] = item.second;
        }
        ToolChanges changes;
        changes.added = wanted;
        Json draft = systemEntry(&sections, &changes, timestamp);
        Json edits = Json::array();
        for (const Json& entry : view.at("entries")) {
            if (m_kinds.is(entry, m_kinds.system())) {
                edits.push_back(Json::object({{"target", entry.at("id")}, {"action", "omit"}}));
            }
        }
        if (!edits.empty()) {
            draft["edits"] = edits;
        }
        return draft;
    }

    bool declarationsEqual(const Json& left, const Json& right) const {
        return left.value("name", Json(nullptr)) == right.value("name", Json(nullptr)) &&
               left.value("description", Json(nullptr)) == right.value("description", Json(nullptr)) &&
               left.value("parameters", Json(nullptr)).dump() == right.value("parameters", Json(nullptr)).dump() &&
               left.value("constrainedSampling", Json(nullptr)).dump() == right.value("constrainedSampling", Json(nullptr)).dump();
    }

    /**
     * Tool changes from `offered` to `desired`. A changed declaration is removed and re-added. Replay keeps retained
     * tools in place and appends additions; when that would not yield the desired order, every offered tool is removed
     * and every desired tool re-added in order.
     */
    ToolChanges planTools(const std::vector<Json>& offered, const std::vector<Json>& desired) const {
        std::vector<Json> kept;
        for (const Json& tool : offered) {
            auto next = std::find_if(desired.begin(), desired.end(), [&](const Json& want) { return want.at("name") == tool.at("name"); });
            if (next != desired.end() && declarationsEqual(tool, *next)) {
                kept.push_back(tool);
            }
        }
        std::vector<Json> added;
        for (const Json& tool : desired) {
            auto isKept = std::find_if(kept.begin(), kept.end(), [&](const Json& item) { return item.at("name") == tool.at("name"); });
            if (isKept == kept.end()) {
                added.push_back(tool);
            }
        }
        std::vector<Json> replayed = kept;
        replayed.insert(replayed.end(), added.begin(), added.end());
        ToolChanges changes;
        bool reordered = false;
        for (std::size_t i = 0; i < replayed.size() && i < desired.size(); ++i) {
            reordered = reordered || replayed[i].at("name") != desired[i].at("name");
        }
        if (reordered) {
            for (const Json& tool : offered) {
                changes.removed.push_back(Json::object({{"name", tool.at("name")}}));
            }
            changes.added = desired;
            return changes;
        }
        for (const Json& tool : offered) {
            auto isKept = std::find_if(kept.begin(), kept.end(), [&](const Json& item) { return item.at("name") == tool.at("name"); });
            if (isKept == kept.end()) {
                changes.removed.push_back(Json::object({{"name", tool.at("name")}}));
            }
        }
        changes.added = added;
        return changes;
    }

    /** Section patches: none, the minimal patch, or a remove-all/re-add-all pair when the order would differ. */
    std::vector<Json> planSections(const Sections& shown, const Sections& desired) const {
        std::vector<std::string> patchedOrder;
        for (const auto& item : shown) {
            if (has(desired, item.first)) {
                patchedOrder.push_back(item.first);
            }
        }
        for (const auto& item : desired) {
            if (!has(shown, item.first)) {
                patchedOrder.push_back(item.first);
            }
        }
        bool reordered = false;
        for (std::size_t i = 0; i < patchedOrder.size() && i < desired.size(); ++i) {
            reordered = reordered || patchedOrder[i] != desired[i].first;
        }
        if (reordered) {
            Json removeAll = Json::object();
            for (const auto& item : shown) {
                removeAll[item.first] = nullptr;
            }
            Json addAll = Json::object();
            for (const auto& item : desired) {
                addAll[item.first] = item.second;
            }
            return {removeAll, addAll};
        }
        Json patch = Json::object();
        for (const auto& item : shown) {
            auto next = std::find_if(desired.begin(), desired.end(), [&](const auto& want) { return want.first == item.first; });
            if (next == desired.end()) {
                patch[item.first] = nullptr;
            } else if (next->second != item.second) {
                patch[item.first] = next->second;
            }
        }
        for (const auto& item : desired) {
            if (!has(shown, item.first)) {
                patch[item.first] = item.second;
            }
        }
        return patch.empty() ? std::vector<Json>() : std::vector<Json>{patch};
    }

    bool has(const Sections& sections, const std::string& key) const {
        return std::any_of(sections.begin(), sections.end(), [&](const auto& item) { return item.first == key; });
    }

    Json systemEntry(const Json* sections, const ToolChanges* tools, std::int64_t timestamp) const {
        Json message = Json::object({{"role", "system"}, {"content", ""}});
        if (sections != nullptr) {
            message["sections"] = *sections;
        }
        if (tools != nullptr && !tools->removed.empty()) {
            message["toolsRemoved"] = tools->removed;
        }
        if (tools != nullptr && !tools->added.empty()) {
            message["toolsAdded"] = tools->added;
        }
        message["timestamp"] = timestamp;
        return Json::object({{"kind", m_kinds.system()}, {"model", Json::array({message})}});
    }

    EntryKinds m_kinds;
};
