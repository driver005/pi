module;

#include <nlohmann/json.hpp>

export module pi.testing.scripted_service_attachment;

import std;
export import pi.server.i_service_attachment;

/** IServiceAttachment whose calls run a test-supplied handler; reports its release. */
export class ScriptedServiceAttachment : public IServiceAttachment {
public:
    using Handler = std::function<Result<std::optional<Json>>(const Json& call, const IServiceEndpoint::Publisher& publish,
                                                              const ServiceContext& context)>;

    ScriptedServiceAttachment(Handler handler, std::function<void()> onRelease);

    Result<std::optional<Json>> invokeService(const Json& call, const IServiceEndpoint::Publisher& publish,
                                              const ServiceContext& context) override;
    void release(const ServiceContext& context) override;

private:
    Handler m_handler;
    std::function<void()> m_onRelease;
};

ScriptedServiceAttachment::ScriptedServiceAttachment(Handler handler, std::function<void()> onRelease)
    : m_handler(std::move(handler)), m_onRelease(std::move(onRelease)) {}

Result<std::optional<Json>> ScriptedServiceAttachment::invokeService(const Json& call,
                                                                     const IServiceEndpoint::Publisher& publish,
                                                                     const ServiceContext& context) {
    if (!m_handler) {
        return std::optional<Json>();
    }
    return m_handler(call, publish, context);
}

void ScriptedServiceAttachment::release(const ServiceContext&) {
    if (m_onRelease) {
        m_onRelease();
    }
}
