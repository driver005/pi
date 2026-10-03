export module pi.types.agent_tool_result;

import std;
export import pi.types.json;
export import pi.types.usage;
export import pi.types.user_content_block;

/** Result of one tool execution (final or partial). */
export struct AgentToolResult {
    /** Text or image content sent back to the model. */
    std::vector<UserContentBlock> content;
    /** Arbitrary structured details for logs and UIs; null means none. */
    Json details;
    /** Machine-readable result for programmatic callers; not sent to the model. */
    Json structuredContent;
    std::optional<Usage> usage;
    /** Report a failure without losing details/structuredContent. */
    bool isError = false;
    /** Stop after this tool batch when every finalized result in it sets this. */
    bool terminate = false;
};
