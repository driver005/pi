#pragma once

#include <optional>
#include <string>
#include <vector>

#include "interfaces/support/json_reader/json_reader.h"
#include "interfaces/types/assistant_content_block/assistant_content_block.h"
#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/deferred_handle/deferred_handle.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/message/message.h"
#include "interfaces/types/nested_tool_calls/nested_tool_calls.h"
#include "interfaces/types/result/result.h"
#include "interfaces/types/stop_reason/stop_reason.h"
#include "interfaces/types/system_message/system_message.h"
#include "interfaces/types/thinking_level/thinking_level.h"
#include "interfaces/types/tool/tool.h"
#include "interfaces/types/tool_result_message/tool_result_message.h"
#include "interfaces/types/usage/usage.h"
#include "interfaces/types/user_content_block/user_content_block.h"
#include "interfaces/types/user_message/user_message.h"

/**
 * JSON <-> C++ conversion for transcript messages. The JSON shape is the one pi has always
 * written to session files and the wire (role-tagged messages, type-tagged content blocks,
 * optional fields omitted when unset), so TypeScript and C++ read each other's data.
 */
class MessageCodec {
public:
    std::string stopReasonName(StopReason reason) const;
    std::optional<StopReason> parseStopReason(const std::string& name) const;
    std::string thinkingLevelName(ThinkingLevel level) const;
    std::optional<ThinkingLevel> parseThinkingLevel(const std::string& name) const;

    Json toJson(const TextContent& block) const;
    Json toJson(const ThinkingContent& block) const;
    Json toJson(const ImageContent& block) const;
    Json toJson(const ToolCall& call) const;
    Json toJson(const UserContentBlock& block) const;
    Json toJson(const AssistantContentBlock& block) const;
    Json toJson(const Usage& usage) const;
    Json toJson(const Tool& tool) const;
    Json toJson(const DeferredHandle& handle) const;
    Json toJson(const NestedToolCalls& nested) const;
    Json toJson(const SystemMessage& message) const;
    Json toJson(const UserMessage& message) const;
    Json toJson(const AssistantMessage& message) const;
    Json toJson(const ToolResultMessage& message) const;
    Json toJson(const Message& message) const;
    Json toJson(const std::vector<Message>& messages) const;

    Result<UserContentBlock> userBlockFromJson(const Json& json) const;
    Result<AssistantContentBlock> assistantBlockFromJson(const Json& json) const;
    Result<ToolCall> toolCallFromJson(const Json& json) const;
    Result<Usage> usageFromJson(const Json& json) const;
    Result<Tool> toolFromJson(const Json& json) const;
    Result<Message> messageFromJson(const Json& json) const;
    Result<AssistantMessage> assistantMessageFromJson(const Json& json) const;
    Result<std::vector<Message>> messagesFromJson(const Json& json) const;

private:
    Result<SystemMessage> systemFromJson(const JsonReader& reader) const;
    Result<UserMessage> userFromJson(const JsonReader& reader) const;
    Result<ToolResultMessage> toolResultFromJson(const JsonReader& reader) const;
    Result<std::vector<UserContentBlock>> userBlocksFromJson(const Json& array,
                                                             const std::string& path) const;
    Result<std::vector<TextContent>> textBlocksFromJson(const Json& array,
                                                        const std::string& path) const;
    Result<std::vector<Tool>> toolsFromJson(const Json& array, const std::string& path) const;
    Result<std::optional<NestedToolCalls>> nestedFromJson(const JsonReader& reader) const;
    std::optional<DeferredHandle> deferredFromJson(const Json& json) const;
    Json userContentToJson(const std::vector<UserContentBlock>& blocks) const;
    void setOptional(Json& object, const char* key, const std::optional<std::string>& value) const;
};
