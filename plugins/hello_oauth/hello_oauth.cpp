// style:c-abi
// An example plugin: registers the model provider "hello-oauth" and its OAuth sign-in. `pi auth login hello-oauth` shows a login
// URL, asks for the code the page displayed and stores a credential for it; the access token is the provider's API key and
// refreshing exchanges the refresh token for a renewed one ("renewed-<refresh>").
#include <chrono>

#include "pi_plugin.hpp"

class HelloOauthPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        std::string error = host.registerProvider(
            "hello-oauth",
            pi::Json{{"baseUrl", "https://hello.example/v1"},
                     {"api", "openai-completions"},
                     {"models", pi::Json::array({pi::Json{{"id", "hello-1"}, {"name", "Hello 1"}, {"contextWindow", 32000}, {"maxTokens", 4096}}})}});
        error += host.registerOauth(
            "hello-oauth", pi::Json{{"name", "Hello (OAuth)"}, {"subscription", true}},
            [](const pi::LoginCall& call) { return login(call); },
            [](const pi::Json& credential, const PiAbort*) { return refresh(credential); });
        if (!error.empty()) {
            host.log(PI_LOG_ERROR, error);
            return false;
        }
        return true;
    }

private:
    static pi::Json login(const pi::LoginCall& call) {
        if (!call.ui.auth("https://hello.example/login", "Open the page and copy the code it shows.")) {
            return pi::failure("sign-in cancelled");
        }
        call.ui.progress("Waiting for the code...");
        const std::string code = call.ui.prompt("Code:");
        if (code.empty()) {
            return pi::failure("no code entered");
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return pi::Json{{"access", "access-" + code}, {"refresh", "refresh-" + code}, {"expires", now + 3600 * 1000}, {"account", code}};
    }

    static pi::Json refresh(const pi::Json& credential) {
        const std::string refresh = credential.value("refresh", "");
        if (refresh.empty()) {
            return pi::failure("no refresh token");
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return pi::Json{{"access", "renewed-" + refresh}, {"refresh", refresh}, {"expires", now + 3600 * 1000}, {"account", credential.value("account", "")}};
    }
};

PI_PLUGIN(HelloOauthPlugin)
