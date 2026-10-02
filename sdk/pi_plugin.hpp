// style:c-abi
// Header-only C++ wrapper over pi_plugin.h for plugin authors. It needs a C++17 compiler and
// nlohmann/json; it throws nothing across the C boundary.
//
//     class Hello : public pi::Plugin {
//       public:
//         void init(pi::Host& host) override {
//             host.registerTool({{"name", "hello"}, {"description", "Greets"},
//                                {"parameters", {{"type", "object"}}}},
//                               [](const pi::ToolCall& call) { return pi::text("hello"); });
//         }
//     };
//     PI_PLUGIN(Hello)
#ifndef PI_PLUGIN_HPP
#define PI_PLUGIN_HPP

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pi_plugin.h"

namespace pi {

using Json = nlohmann::json;

/** One tool invocation as the plugin sees it. */
struct ToolCall {
    std::string id;
    Json params;
    const PiAbort* abort = nullptr;
    PiUpdateFn onUpdate = nullptr;
    void* updateContext = nullptr;
    const PiHostApi* host = nullptr;

    bool aborted() const { return host != nullptr && host->abort_requested(abort) != 0; }

    /** Streams a partial result to the model/UI. */
    void update(const std::string& text) const {
        if (onUpdate == nullptr) {
            return;
        }
        const std::string json = Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}}.dump();
        onUpdate(updateContext, PiString{json.data(), json.size()});
    }
};

using ToolHandler = std::function<Json(const ToolCall&)>;
using HookHandler = std::function<Json(const Json& payload)>;

/** A text-only tool result. */
inline Json text(const std::string& value, bool isError = false) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", value}}})}, {"isError", isError}};
}

/** A failed call; the model sees the message. */
inline Json failure(const std::string& message) {
    return Json{{"error", message}};
}

namespace detail {

inline PiOwnedString own(const std::string& value) {
    char* copy = new char[value.size()];
    std::memcpy(copy, value.data(), value.size());
    return PiOwnedString{copy, value.size(), [](char* data, std::size_t) { delete[] data; }};
}

inline std::string take(PiOwnedString value) {
    std::string out = value.data != nullptr ? std::string(value.data, value.size) : std::string();
    if (value.release != nullptr) {
        value.release(value.data, value.size);
    }
    return out;
}

}  // namespace detail

class Host {
public:
    explicit Host(const PiHostApi* api) : m_api(api) {}

    const PiHostApi* api() const { return m_api; }

    void log(int level, const std::string& message) const {
        m_api->log(m_api->host, level, PiString{message.data(), message.size()});
    }

    /** Returns an empty string on success, else the host's error message. */
    std::string registerTool(const Json& definition, ToolHandler handler) {
        m_entries.push_back(std::make_unique<Entry>(Entry{std::move(handler), m_api}));
        Entry* entry = m_entries.back().get();
        const std::string text = definition.dump();
        const Json reply = Json::parse(
            detail::take(m_api->register_tool(
                m_api->host, PiString{text.data(), text.size()},
                [](void* userData, PiString id, PiString params, const PiAbort* abort, PiUpdateFn onUpdate,
                   void* updateContext) -> PiOwnedString {
                    auto* entry = static_cast<Entry*>(userData);
                    ToolCall call;
                    call.id.assign(id.data, id.size);
                    call.params = Json::parse(std::string(params.data, params.size), nullptr, false);
                    call.abort = abort;
                    call.onUpdate = onUpdate;
                    call.updateContext = updateContext;
                    call.host = entry->host;
                    return detail::own(entry->handler(call).dump());
                },
                entry)),
            nullptr, false);
        return reply.value("error", "");
    }

    /** Returns an empty string on success, else the host's error message. */
    std::string on(const std::string& event, HookHandler handler) {
        m_hooks.push_back(std::make_unique<HookHandler>(std::move(handler)));
        const Json reply = Json::parse(
            detail::take(m_api->subscribe(
                m_api->host, PiString{event.data(), event.size()},
                [](void* userData, PiString, PiString payload) -> PiOwnedString {
                    const Json result = (*static_cast<HookHandler*>(userData))(
                        Json::parse(std::string(payload.data, payload.size), nullptr, false));
                    return result.is_null() ? PiOwnedString{nullptr, 0, nullptr} : detail::own(result.dump());
                },
                m_hooks.back().get())),
            nullptr, false);
        return reply.value("error", "");
    }

    Json context() const { return Json::parse(detail::take(m_api->get_context(m_api->host)), nullptr, false); }

    /** Runs a program; see PiHostApi.exec for the request and result shapes. */
    Json exec(const Json& request, const PiAbort* abort = nullptr) const {
        const std::string text = request.dump();
        return Json::parse(detail::take(m_api->exec(m_api->host, PiString{text.data(), text.size()}, abort)), nullptr,
                           false);
    }

private:
    struct Entry {
        ToolHandler handler;
        const PiHostApi* host;
    };

    const PiHostApi* m_api;
    std::vector<std::unique_ptr<HookHandler>> m_hooks;
    std::vector<std::unique_ptr<Entry>> m_entries;
};

/** Derive from this and register the class with PI_PLUGIN. */
class Plugin {
public:
    virtual ~Plugin() = default;
    /** Registers tools and hooks; return false to abort loading. */
    virtual bool init(Host& host) = 0;
    virtual void shutdown() {}
};

}  // namespace pi

#define PI_PLUGIN(PluginClass)                                                              \
    namespace {                                                                             \
    std::unique_ptr<pi::Host> g_piHost;                                                     \
    std::unique_ptr<pi::Plugin> g_piPlugin;                                                 \
    }                                                                                       \
    extern "C" uint32_t pi_plugin_abi_version(void) { return PI_PLUGIN_ABI_VERSION; }       \
    extern "C" int pi_plugin_init(const PiHostApi* api) {                                   \
        g_piHost = std::make_unique<pi::Host>(api);                                         \
        g_piPlugin = std::make_unique<PluginClass>();                                       \
        return g_piPlugin->init(*g_piHost) ? 0 : 1;                                         \
    }                                                                                       \
    extern "C" void pi_plugin_shutdown(void) {                                              \
        if (g_piPlugin) {                                                                   \
            g_piPlugin->shutdown();                                                         \
        }                                                                                   \
        g_piPlugin.reset();                                                                 \
        g_piHost.reset();                                                                   \
    }

#endif
