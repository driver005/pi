export module pi.types.prompt_options;

import std;
export import pi.types.image_content;
export import pi.types.prompt_disposition;

/** How a prompt reaches the agent while another run may be active. */
export enum class StreamingBehavior { None, Steer, FollowUp };

export struct PromptOptions {
    /** Expand /skill:name commands and prompt templates. */
    bool expandPromptTemplates = true;
    std::vector<ImageContent> images;
    /** Where the input came from, as reported to `input` plugin handlers: "interactive", "rpc" or "extension". */
    std::string source = "rpc";
    /** Required when the agent is already running: queue as steering or follow-up. */
    StreamingBehavior streamingBehavior = StreamingBehavior::None;
    /** Called once the prompt is accepted (before a started run begins), never when it is rejected. */
    std::function<void(PromptDisposition)> onDisposition;
};
