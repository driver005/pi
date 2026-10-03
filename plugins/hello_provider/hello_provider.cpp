// An example plugin: registers the model provider "hello-proxy" (one OpenAI-compatible model behind a proxy URL).
#include "pi_plugin.hpp"

class HelloProviderPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        const std::string error = host.registerProvider(
            "hello-proxy",
            pi::Json{{"baseUrl", "https://proxy.example.com/v1"},
                     {"apiKey", "hello-key"},
                     {"api", "openai-completions"},
                     {"models", pi::Json::array({pi::Json{{"id", "hello-1"}, {"name", "Hello 1"}, {"contextWindow", 32000}, {"maxTokens", 4096}}})}});
        if (!error.empty()) {
            host.log(PI_LOG_ERROR, error);
            return false;
        }
        return true;
    }
};

PI_PLUGIN(HelloProviderPlugin)
