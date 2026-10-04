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

#include <cstddef>
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

/** One request of a virtual model as the router sees it (see PiRouteFn for the fields of `request`). */
struct RouteCall {
    Json request;
    const PiAbort* abort = nullptr;
    const PiHostApi* host = nullptr;

    bool aborted() const { return host != nullptr && host->abort_requested(abort) != 0; }
};

/** Returns {"model":{"provider","id"},"thinkingLevel":"low","state"?:any} or {"error":"message"}. */
using RouteHandler = std::function<Json(const RouteCall&)>;

/** Reports the response of a stream-handler provider (see PiHostApi.stream_emit); each call returns false once the response is over. */
class StreamSink {
public:
    StreamSink(const PiHostApi* api, PiStreamSink* sink) : m_api(api), m_sink(sink) {}

    bool text(const std::string& delta) const { return emit(Json{{"type", "text_delta"}, {"delta", delta}}); }
    bool thinking(const std::string& delta) const { return emit(Json{{"type", "thinking_delta"}, {"delta", delta}}); }
    bool toolCall(const std::string& id, const std::string& name, const Json& arguments) const {
        return emit(Json{{"type", "tool_call"}, {"id", id}, {"name", name}, {"arguments", arguments}});
    }
    bool usage(long long input, long long output, long long cacheRead = 0, long long cacheWrite = 0) const {
        return emit(Json{{"type", "usage"}, {"input", input}, {"output", output}, {"cacheRead", cacheRead}, {"cacheWrite", cacheWrite}});
    }
    bool response(const std::string& id, const std::string& model = "") const {
        Json event{{"type", "response"}, {"id", id}};
        if (!model.empty()) {
            event["model"] = model;
        }
        return emit(event);
    }
    /** Ends the response; stopReason is "stop", "length" or "toolUse" (empty: "toolUse" after tool calls, else "stop"). */
    bool done(const std::string& stopReason = "") const {
        Json event{{"type", "done"}};
        if (!stopReason.empty()) {
            event["stopReason"] = stopReason;
        }
        return emit(event);
    }
    bool error(const std::string& message, bool aborted = false) const {
        return emit(Json{{"type", "error"}, {"message", message}, {"aborted", aborted}});
    }
    /** Sends a raw event (see PiHostApi.stream_emit). */
    bool emit(const Json& event) const {
        const std::string text = event.dump();
        return m_api->stream_emit(m_sink, PiString{text.data(), text.size()}) == 0;
    }

private:
    const PiHostApi* m_api;
    PiStreamSink* m_sink;
};

/** One request of a stream-handler provider (see PiStreamFn for the fields of `request`). */
struct StreamCall {
    Json request;
    const PiAbort* abort = nullptr;
    const PiHostApi* host = nullptr;

    bool aborted() const { return host != nullptr && host->abort_requested(abort) != 0; }
};

/** Streams one response through the sink and ends it with done() or error(). Runs on a thread of its own. */
using StreamHandler = std::function<void(const StreamCall&, const StreamSink&)>;

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

    /**
     * Registers a model provider (see PiHostApi.register_provider). Returns an empty string on success, else the host's
     * error message; hosts older than this call report that it is unavailable.
     */
    std::string registerProvider(const std::string& name, const Json& config) const {
        if (!hasProviderApi()) {
            return "the host does not support register_provider";
        }
        const std::string text = config.dump();
        return Json::parse(detail::take(m_api->register_provider(m_api->host, PiString{name.data(), name.size()}, PiString{text.data(), text.size()})), nullptr, false)
            .value("error", "");
    }

    /**
     * Registers a provider that streams its responses itself (see PiHostApi.register_stream_provider): `config` names its
     * "api" and models; `handler` runs once per request. Returns an empty string on success, else the host's error message.
     */
    std::string registerStreamProvider(const std::string& name, const Json& config, StreamHandler handler) {
        if (!hasStreamApi()) {
            return "the host does not support register_stream_provider";
        }
        m_streams.push_back(std::make_unique<StreamEntry>(StreamEntry{std::move(handler), m_api}));
        StreamEntry* entry = m_streams.back().get();
        const std::string text = config.dump();
        return Json::parse(
                   detail::take(m_api->register_stream_provider(
                       m_api->host, PiString{name.data(), name.size()}, PiString{text.data(), text.size()},
                       [](void* userData, PiString request, const PiAbort* abort, PiStreamSink* sink) {
                           auto* entry = static_cast<StreamEntry*>(userData);
                           StreamCall call;
                           call.request = Json::parse(std::string(request.data, request.size), nullptr, false);
                           call.abort = abort;
                           call.host = entry->host;
                           entry->handler(call, StreamSink(entry->host, sink));
                       },
                       entry)),
                   nullptr, false)
            .value("error", "");
    }

    /** Removes a provider this plugin registered. Returns an empty string on success. */
    std::string unregisterProvider(const std::string& name) const {
        if (!hasProviderApi()) {
            return "the host does not support unregister_provider";
        }
        return Json::parse(detail::take(m_api->unregister_provider(m_api->host, PiString{name.data(), name.size()})), nullptr, false).value("error", "");
    }

    /** Runs a program; see PiHostApi.exec for the request and result shapes. */
    Json exec(const Json& request, const PiAbort* abort = nullptr) const {
        const std::string text = request.dump();
        return Json::parse(detail::take(m_api->exec(m_api->host, PiString{text.data(), text.size()}, abort)), nullptr,
                           false);
    }

    /**
     * Registers an MCP server for the session (see PiHostApi.register_mcp_server). Returns an empty string on success,
     * else the host's error message.
     */
    std::string registerMcpServer(const std::string& name, const Json& config) const {
        if (!hasMcpApi()) {
            return "the host does not support register_mcp_server";
        }
        const std::string text = config.dump();
        return Json::parse(detail::take(m_api->register_mcp_server(m_api->host, PiString{name.data(), name.size()}, PiString{text.data(), text.size()})), nullptr, false)
            .value("error", "");
    }

    /** Removes an MCP server this plugin registered. Returns an empty string on success. */
    std::string unregisterMcpServer(const std::string& name) const {
        if (!hasMcpApi()) {
            return "the host does not support unregister_mcp_server";
        }
        return Json::parse(detail::take(m_api->unregister_mcp_server(m_api->host, PiString{name.data(), name.size()})), nullptr, false).value("error", "");
    }

    /**
     * Registers a virtual model (see PiHostApi.register_virtual_model): `route` picks the physical model of each request.
     * Returns an empty string on success, else the host's error message.
     */
    std::string registerVirtualModel(const Json& definition, RouteHandler route) {
        if (!hasVirtualModelApi()) {
            return "the host does not support register_virtual_model";
        }
        m_routes.push_back(std::make_unique<RouteEntry>(RouteEntry{std::move(route), m_api}));
        RouteEntry* entry = m_routes.back().get();
        const std::string text = definition.dump();
        return Json::parse(
                   detail::take(m_api->register_virtual_model(
                       m_api->host, PiString{text.data(), text.size()},
                       [](void* userData, PiString request, const PiAbort* abort) -> PiOwnedString {
                           auto* entry = static_cast<RouteEntry*>(userData);
                           RouteCall call;
                           call.request = Json::parse(std::string(request.data, request.size), nullptr, false);
                           call.abort = abort;
                           call.host = entry->host;
                           return detail::own(entry->handler(call).dump());
                       },
                       entry)),
                   nullptr, false)
            .value("error", "");
    }

    /** Removes a virtual model this plugin registered. Returns an empty string on success. */
    std::string unregisterVirtualModel(const std::string& provider, const std::string& id) const {
        if (!hasVirtualModelApi()) {
            return "the host does not support unregister_virtual_model";
        }
        return Json::parse(detail::take(m_api->unregister_virtual_model(m_api->host, PiString{provider.data(), provider.size()}, PiString{id.data(), id.size()})), nullptr, false)
            .value("error", "");
    }

    /** The physical models whose provider has credentials (see PiHostApi.list_models); empty on older hosts. */
    Json models() const {
        if (!hasVirtualModelApi()) {
            return Json::array();
        }
        return Json::parse(detail::take(m_api->list_models(m_api->host)), nullptr, false);
    }

private:
    bool hasVirtualModelApi() const {
        return m_api->struct_size >= offsetof(PiHostApi, list_models) + sizeof(void*) && m_api->register_virtual_model != nullptr &&
               m_api->unregister_virtual_model != nullptr && m_api->list_models != nullptr;
    }

    bool hasMcpApi() const {
        return m_api->struct_size >= offsetof(PiHostApi, unregister_mcp_server) + sizeof(void*) && m_api->register_mcp_server != nullptr &&
               m_api->unregister_mcp_server != nullptr;
    }

    bool hasStreamApi() const {
        return m_api->struct_size >= offsetof(PiHostApi, stream_emit) + sizeof(void*) && m_api->register_stream_provider != nullptr &&
               m_api->stream_emit != nullptr;
    }

    bool hasProviderApi() const {
        return m_api->struct_size >= offsetof(PiHostApi, unregister_provider) + sizeof(void*) && m_api->register_provider != nullptr &&
               m_api->unregister_provider != nullptr;
    }

    struct Entry {
        ToolHandler handler;
        const PiHostApi* host;
    };

    struct RouteEntry {
        RouteHandler handler;
        const PiHostApi* host;
    };

    struct StreamEntry {
        StreamHandler handler;
        const PiHostApi* host;
    };

    const PiHostApi* m_api;
    std::vector<std::unique_ptr<HookHandler>> m_hooks;
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<std::unique_ptr<RouteEntry>> m_routes;
    std::vector<std::unique_ptr<StreamEntry>> m_streams;
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
