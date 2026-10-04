export module pi.types.rpc_target;

import std;

/** What a request or cancel addresses: the server itself, or one session attachment on it. */
export struct RpcTarget {
    std::string serverId;
    std::optional<std::string> sessionId;
    std::optional<std::string> attachmentId;
};
