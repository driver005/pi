export module pi.server.i_routed_session_handle;

import std;
export import pi.server.i_service_attachment;
export import pi.types.error;
export import pi.types.result;
export import pi.types.service_context;

/** A handle to one opened session that connections attach to. Thread-safe. */
export class IRoutedSessionHandle {
public:
    using TerminationListener = std::function<void(const std::optional<Error>& failure)>;

    virtual ~IRoutedSessionHandle() = default;

    virtual Result<std::unique_ptr<IServiceAttachment>> attachClient(const ServiceContext& context) = 0;
    /** Called once when the session ends on its own: an Error for an unexpected end. */
    virtual void onTermination(TerminationListener listener) = 0;
    virtual Result<void> close(const ServiceContext& context) = 0;
};
