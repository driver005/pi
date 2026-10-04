// style:c-abi
// An example plugin: registers the provider "hello-stream" whose model "echo" streams its own responses. It answers a user
// message with "You said: <text>" word by word; the message "call <tool>" makes it call that tool with {"input": <rest>}, the
// message "fail" ends the response with an error, and "slow" streams until the request is cancelled. After a tool ran it
// answers "The tool answered.".
#include <chrono>
#include <thread>

#include "pi_plugin.hpp"

class HelloStreamPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        const std::string error = host.registerStreamProvider(
            "hello-stream",
            pi::Json{{"api", "hello-stream-api"},
                     {"apiKey", "none"},
                     {"baseUrl", "https://hello-stream.invalid"},
                     {"models", pi::Json::array({pi::Json{{"id", "echo"}, {"name", "Echo"}, {"contextWindow", 32000}, {"maxTokens", 4096}}})}},
            [](const pi::StreamCall& call, const pi::StreamSink& sink) { respond(call, sink); });
        if (!error.empty()) {
            host.log(PI_LOG_ERROR, error);
            return false;
        }
        return true;
    }

private:
    /** The text of the last user message of the request. */
    static std::string lastUserText(const pi::Json& request) {
        const pi::Json& messages = request["messages"];
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if ((*it).value("role", "") != "user") {
                continue;
            }
            const pi::Json& content = (*it)["content"];
            if (content.is_string()) {
                return content.get<std::string>();
            }
            for (const pi::Json& block : content) {
                if (block.value("type", "") == "text") {
                    return block.value("text", "");
                }
            }
        }
        return "";
    }

    static bool endsWithToolResult(const pi::Json& request) {
        const pi::Json& messages = request["messages"];
        return !messages.empty() && messages.back().value("role", "") == "toolResult";
    }

    static void respond(const pi::StreamCall& call, const pi::StreamSink& sink) {
        if (endsWithToolResult(call.request)) {
            sink.text("The tool answered.");
            sink.done();
            return;
        }
        const std::string text = lastUserText(call.request);
        if (text == "fail") {
            sink.error("the echo model failed");
            return;
        }
        if (text == "slow") {
            while (!call.aborted() && sink.text(".")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            sink.error("cancelled", true);
            return;
        }
        if (text.rfind("call ", 0) == 0) {
            const std::string rest = text.substr(5);
            const std::size_t space = rest.find(' ');
            sink.toolCall("call-1", rest.substr(0, space), pi::Json{{"input", space == std::string::npos ? "" : rest.substr(space + 1)}});
            sink.usage(10, 5);
            sink.done();
            return;
        }
        std::string word;
        sink.thinking("echoing");
        for (const char c : "You said: " + text + " ") {
            if (c == ' ') {
                if (!word.empty() && !sink.text(word + " ")) {
                    return;
                }
                word.clear();
            } else {
                word.push_back(c);
            }
        }
        sink.usage(10, 5);
        sink.response("echo-1", "echo");
        sink.done();
    }
};

PI_PLUGIN(HelloStreamPlugin)
