export module pi.support.copilot_headers;

import std;
export import pi.types.http_headers;
export import pi.types.message;

/** Per-request headers GitHub Copilot expects. Port of api/github-copilot-headers.ts. */
export class CopilotHeaders {
public:
    /** "agent" when the last message is not from the user, else "user". */
    std::string initiator(const std::vector<Message>& messages) const;
    bool hasVisionInput(const std::vector<Message>& messages) const;
    HttpHeaders dynamicHeaders(const std::vector<Message>& messages) const;

private:
    bool hasImage(const std::vector<UserContentBlock>& blocks) const;
};

bool CopilotHeaders::hasImage(const std::vector<UserContentBlock>& blocks) const {
    return std::any_of(blocks.begin(), blocks.end(), [](const UserContentBlock& block) {
        return std::holds_alternative<ImageContent>(block);
    });
}

std::string CopilotHeaders::initiator(const std::vector<Message>& messages) const {
    if (messages.empty() || std::holds_alternative<UserMessage>(messages.back())) {
        return "user";
    }
    return "agent";
}

bool CopilotHeaders::hasVisionInput(const std::vector<Message>& messages) const {
    for (const auto& message : messages) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (const auto* blocks = std::get_if<std::vector<UserContentBlock>>(&user->content)) {
                if (hasImage(*blocks)) {
                    return true;
                }
            }
        } else if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            if (hasImage(result->content)) {
                return true;
            }
        }
    }
    return false;
}

HttpHeaders CopilotHeaders::dynamicHeaders(const std::vector<Message>& messages) const {
    HttpHeaders headers = {{"X-Initiator", initiator(messages)},
                           {"Openai-Intent", "conversation-edits"}};
    if (hasVisionInput(messages)) {
        headers.emplace_back("Copilot-Vision-Request", "true");
    }
    return headers;
}
