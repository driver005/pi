export module pi.support.provider_error_formatter;

import std;
export import pi.types.http_response;
export import pi.types.json;

/** Display strings for failed provider requests. Port of utils/error-body.ts. */
export class ProviderErrorFormatter {
public:
    static constexpr std::size_t MaxBodyChars = 4000;

    /** "<status>: <body>" or "<prefix> (<status>): <body>"; body trimmed and truncated. */
    std::string formatHttp(const HttpResponse& response, const std::string& prefix = "") const {
        const std::string body = truncate(trim(response.body), MaxBodyChars);
        const std::string status = std::to_string(response.status);
        if (body.empty()) {
            return prefix.empty() ? status + " status code (no body)"
                                  : prefix + " (" + status + "): status code (no body)";
        }
        return prefix.empty() ? status + ": " + body : prefix + " (" + status + "): " + body;
    }

    /**
     * Message in the vendor SDK style: "<status> <message>" when the JSON body has a top-level
     * message string, "<status> <body json>" for other bodies, "<status> status code (no body)".
     */
    std::string formatSdk(const HttpResponse& response) const {
        const std::string status = std::to_string(response.status);
        const std::string body = trim(response.body);
        if (body.empty()) {
            return status + " status code (no body)";
        }
        const Json json = Json::parse(body, nullptr, false);
        if (json.is_object() && json.contains("message") && json["message"].is_string()) {
            return status + " " + json["message"].get<std::string>();
        }
        return status + " " + truncate(body, MaxBodyChars);
    }

    /**
     * OpenAI SDK style: the message comes from the body's `error` object (message string or its
     * JSON), plus OpenRouter's `error.metadata.raw` detail on its own line when present.
     */
    std::string formatOpenAi(const HttpResponse& response) const {
        const std::string status = std::to_string(response.status);
        const std::string body = trim(response.body);
        if (body.empty()) {
            return status + " status code (no body)";
        }
        const Json json = Json::parse(body, nullptr, false);
        if (!json.is_object()) {
            return status + " " + truncate(body, MaxBodyChars);
        }
        const auto dump = [](const Json& value) {
            return value.dump(-1, ' ', false, Json::error_handler_t::replace);
        };
        const Json error = json.contains("error") ? json["error"] : json;
        std::string message;
        if (error.is_string()) {
            message = error.get<std::string>();
        } else if (error.is_object() && error.contains("message") && error["message"].is_string()) {
            message = error["message"].get<std::string>();
        } else {
            message = truncate(dump(error), MaxBodyChars);
        }
        std::string out = status + " " + message;
        if (error.is_object() && error.contains("metadata") && error["metadata"].is_object() &&
            error["metadata"].contains("raw") && error["metadata"]["raw"].is_string()) {
            const std::string raw = error["metadata"]["raw"].get<std::string>();
            if (out.find(raw) == std::string::npos) {
                out += "\n" + raw;
            }
        }
        return out;
    }

    /** Appends "... [truncated N chars]" past the limit, without splitting a UTF-8 sequence. */
    std::string truncate(const std::string& text, std::size_t maxChars) const {
        if (text.size() <= maxChars) {
            return text;
        }
        std::size_t cut = maxChars;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        return text.substr(0, cut) + "... [truncated " + std::to_string(text.size() - cut) + " chars]";
    }

    /** Message from {"error":{"message":..}}, {"message":..} or {"error":".."}; else empty. */
    std::string extractMessage(const std::string& body) const {
        const Json json = Json::parse(body, nullptr, false);
        if (!json.is_object()) {
            return "";
        }
        if (json.contains("error")) {
            const Json& error = json["error"];
            if (error.is_string()) {
                return error.get<std::string>();
            }
            if (error.is_object() && error.contains("message") && error["message"].is_string()) {
                return error["message"].get<std::string>();
            }
        }
        if (json.contains("message") && json["message"].is_string()) {
            return json["message"].get<std::string>();
        }
        return "";
    }

private:
    std::string trim(const std::string& text) const {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }
};
