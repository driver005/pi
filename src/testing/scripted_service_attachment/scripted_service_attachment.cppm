export module pi.testing.scripted_service_attachment;

import std;
export import pi.server.i_service_attachment;

/** IServiceAttachment whose calls run a test-supplied handler; reports its release. */
export class ScriptedServiceAttachment : public IServiceAttachment {
public:
    using Handler = std::function<Result<std::optional<Json>>(const Json& call, const IServiceEndpoint::Publisher& publish,
                                                              const ServiceContext& context)>;

    ScriptedServiceAttachment(Handler handler, std::function<void()> onRelease)
        : m_handler(std::move(handler)),
          m_onRelease(std::move(onRelease)) {}

    Result<std::optional<Json>> invokeService(const Json& call, const IServiceEndpoint::Publisher& publish, const ServiceContext& context) override {
        if (!m_handler) {
            return std::optional<Json>();
        }
        return m_handler(call, publish, context);
    }

    void release(const ServiceContext&) override {
        if (m_onRelease) {
            m_onRelease();
        }
    }

private:
    Handler m_handler;
    std::function<void()> m_onRelease;
};
