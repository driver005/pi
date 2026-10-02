module;

#include <nlohmann/json.hpp>

export module pi.support.session_data_codec;

import std;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.support.model_codec;
export import pi.types.agent_tool_result;
export import pi.types.bash_result;
export import pi.types.compaction_result;
export import pi.types.forkable_message;
export import pi.types.json;
export import pi.types.model_cycle_result;
export import pi.types.queued_input;
export import pi.types.session_stats;
export import pi.types.session_tree_node;
export import pi.types.slash_command_info;
export import pi.types.tool_info;

/** JSON of the values session operations return, in the shape the TypeScript clients read. */
export class SessionDataCodec {
public:
    Json bash(const BashResult& result) const;
    Json compaction(const CompactionResult& result) const;
    Json stats(const SessionStats& stats) const;
    Json treeNode(const SessionTreeNode& node) const;
    Json tree(const std::vector<SessionTreeNode>& nodes) const;
    Json queued(const QueuedInput& input) const;
    Json forkable(const std::vector<ForkableMessage>& messages) const;
    Json slashCommands(const std::vector<SlashCommandInfo>& commands) const;
    Json tools(const std::vector<ToolInfo>& tools) const;
    Json modelCycle(const ModelCycleResult& result) const;
    Json toolResult(const AgentToolResult& result) const;
    Json strings(const std::vector<std::string>& values) const;
    Json optionalString(const std::optional<std::string>& value) const;

private:
    Json sourceInfo(const SourceInfo& info) const;

    MessageCodec m_messages;
    ModelCodec m_models;
};

Json SessionDataCodec::strings(const std::vector<std::string>& values) const {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

Json SessionDataCodec::optionalString(const std::optional<std::string>& value) const {
    return value ? Json(*value) : Json(nullptr);
}

Json SessionDataCodec::bash(const BashResult& result) const {
    Json out = Json::object();
    out["output"] = result.output;
    if (result.exitCode) {
        out["exitCode"] = *result.exitCode;
    }
    out["cancelled"] = result.cancelled;
    out["truncated"] = result.truncated;
    if (result.fullOutputPath) {
        out["fullOutputPath"] = *result.fullOutputPath;
    }
    return out;
}

Json SessionDataCodec::compaction(const CompactionResult& result) const {
    Json out = Json::object();
    out["summary"] = result.summary;
    out["firstKeptEntryId"] = result.firstKeptEntryId;
    out["tokensBefore"] = result.tokensBefore;
    if (result.estimatedTokensAfter) {
        out["estimatedTokensAfter"] = *result.estimatedTokensAfter;
    }
    if (result.usage) {
        out["usage"] = m_messages.toJson(*result.usage);
    }
    if (!result.details.is_null()) {
        out["details"] = result.details;
    }
    return out;
}

Json SessionDataCodec::stats(const SessionStats& stats) const {
    Json out = Json::object();
    if (stats.sessionFile) {
        out["sessionFile"] = *stats.sessionFile;
    }
    out["sessionId"] = stats.sessionId;
    out["userMessages"] = stats.userMessages;
    out["assistantMessages"] = stats.assistantMessages;
    out["toolCalls"] = stats.toolCalls;
    out["toolResults"] = stats.toolResults;
    out["totalMessages"] = stats.totalMessages;
    out["tokens"] = Json{{"input", stats.inputTokens},
                         {"output", stats.outputTokens},
                         {"cacheRead", stats.cacheReadTokens},
                         {"cacheWrite", stats.cacheWriteTokens},
                         {"total", stats.totalTokens}};
    out["cost"] = stats.cost;
    if (stats.contextUsage) {
        Json usage = Json::object();
        usage["tokens"] = stats.contextUsage->tokens ? Json(*stats.contextUsage->tokens) : Json(nullptr);
        usage["contextWindow"] = stats.contextUsage->contextWindow;
        usage["percent"] = stats.contextUsage->percent ? Json(*stats.contextUsage->percent) : Json(nullptr);
        out["contextUsage"] = usage;
    }
    return out;
}

Json SessionDataCodec::treeNode(const SessionTreeNode& node) const {
    Json out = Json::object();
    out["entry"] = node.entry.body;
    Json children = Json::array();
    for (const auto& child : node.children) {
        children.push_back(treeNode(child));
    }
    out["children"] = children;
    if (node.label) {
        out["label"] = *node.label;
    }
    if (node.labelTimestamp) {
        out["labelTimestamp"] = *node.labelTimestamp;
    }
    return out;
}

Json SessionDataCodec::tree(const std::vector<SessionTreeNode>& nodes) const {
    Json out = Json::array();
    for (const auto& node : nodes) {
        out.push_back(treeNode(node));
    }
    return out;
}

Json SessionDataCodec::queued(const QueuedInput& input) const {
    return Json{{"steering", strings(input.steering)}, {"followUp", strings(input.followUp)}};
}

Json SessionDataCodec::forkable(const std::vector<ForkableMessage>& messages) const {
    Json out = Json::array();
    for (const auto& message : messages) {
        out.push_back(Json{{"entryId", message.entryId}, {"text", message.text}});
    }
    return out;
}

Json SessionDataCodec::sourceInfo(const SourceInfo& info) const {
    Json out = Json::object();
    out["path"] = info.path;
    out["source"] = info.source;
    out["scope"] = info.scope;
    out["origin"] = info.origin;
    if (info.baseDir) {
        out["baseDir"] = *info.baseDir;
    }
    return out;
}

Json SessionDataCodec::slashCommands(const std::vector<SlashCommandInfo>& commands) const {
    Json out = Json::array();
    for (const auto& command : commands) {
        Json entry = Json::object();
        entry["name"] = command.name;
        if (!command.description.empty()) {
            entry["description"] = command.description;
        }
        entry["source"] = command.source;
        entry["sourceInfo"] = sourceInfo(command.sourceInfo);
        out.push_back(entry);
    }
    return out;
}

Json SessionDataCodec::tools(const std::vector<ToolInfo>& tools) const {
    Json out = Json::array();
    for (const auto& tool : tools) {
        out.push_back(Json{{"name", tool.name},
                           {"description", tool.description},
                           {"parameters", tool.parameters},
                           {"promptGuidelines", strings(tool.promptGuidelines)},
                           {"active", tool.active}});
    }
    return out;
}

Json SessionDataCodec::modelCycle(const ModelCycleResult& result) const {
    return Json{{"model", m_models.toJson(result.model)},
                {"thinkingLevel", m_messages.thinkingLevelName(result.thinkingLevel)},
                {"isScoped", result.isScoped}};
}

Json SessionDataCodec::toolResult(const AgentToolResult& result) const {
    Json out = Json::object();
    Json content = Json::array();
    for (const auto& block : result.content) {
        content.push_back(m_messages.toJson(block));
    }
    out["content"] = content;
    out["details"] = result.details.is_null() ? Json::object() : result.details;
    if (!result.structuredContent.is_null()) {
        out["structuredContent"] = result.structuredContent;
    }
    if (result.usage) {
        out["usage"] = m_messages.toJson(*result.usage);
    }
    if (result.terminate) {
        out["terminate"] = true;
    }
    return out;
}
