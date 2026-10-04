export module pi.types.assistant_content_block;

import std;
export import pi.types.text_content;
export import pi.types.thinking_content;
export import pi.types.tool_call;

/** Content produced by the model. */
export using AssistantContentBlock = std::variant<TextContent, ThinkingContent, ToolCall>;
