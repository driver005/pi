module;

#include <cstdint>

export module pi.support.mcp_message_codec;

import std;
export import pi.types.json;

/** JSON-RPC 2.0 message construction and classification for MCP. */
export class McpMessageCodec {
public:
    static constexpr int ParseError = -32700;
    static constexpr int InvalidRequest = -32600;
    static constexpr int MethodNotFound = -32601;
    static constexpr int InvalidParams = -32602;
    static constexpr int InternalError = -32603;

    Json request(std::int64_t id, const std::string& method, const Json& params) const {
        Json message = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) {
            message["params"] = params;
        }
        return message;
    }

    Json notification(const std::string& method, const Json& params) const {
        Json message = Json{{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) {
            message["params"] = params;
        }
        return message;
    }

    Json result(const Json& id, const Json& value) const {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", value.is_null() ? Json::object() : value}};
    }

    Json error(const Json& id, int code, const std::string& message) const {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}};
    }

    bool isRequest(const Json& message) const {
        return message.is_object() && message.value("jsonrpc", "") == "2.0" && message.contains("id") &&
               validId(message["id"]) && message.contains("method") && message["method"].is_string();
    }

    bool isNotification(const Json& message) const {
        return message.is_object() && message.value("jsonrpc", "") == "2.0" && !message.contains("id") &&
               message.contains("method") && message["method"].is_string();
    }

    bool isResponse(const Json& message) const {
        if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("id") ||
            !validId(message["id"])) {
            return false;
        }
        if (message.contains("result")) {
            return !message.contains("error");
        }
        return message.contains("error") && message["error"].is_object() && message["error"].contains("code") &&
               message["error"]["code"].is_number() && message["error"].contains("message") &&
               message["error"]["message"].is_string();
    }

    /** "rpc:<code>" error code string for a JSON-RPC error object. */
    std::string rpcCode(int code) const {
        return "rpc:" + std::to_string(code);
    }

    /** The numeric JSON-RPC code of an Error built with rpcCode(), if it is one. */
    std::optional<int> rpcNumber(const std::string& errorCode) const {
        if (!errorCode.starts_with("rpc:")) {
            return std::nullopt;
        }
        int value = 0;
        const char* begin = errorCode.data() + 4;
        const char* end = errorCode.data() + errorCode.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc() || parsed.ptr != end) {
            return std::nullopt;
        }
        return value;
    }

private:
    bool validId(const Json& id) const {
        return id.is_string() || id.is_number();
    }
};
