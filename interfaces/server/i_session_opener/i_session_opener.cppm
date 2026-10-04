export module pi.server.i_session_opener;

import std;
export import pi.server.i_routed_session_handle;
export import pi.types.result;
export import pi.types.service_context;
export import pi.types.session_record;

/** Brings a cataloged session to life: its agent, tools and service endpoints. */
export class ISessionOpener {
public:
    virtual ~ISessionOpener() = default;

    virtual Result<std::shared_ptr<IRoutedSessionHandle>> open(const SessionRecord& record,
                                                               const ServiceContext& context) = 0;
};
