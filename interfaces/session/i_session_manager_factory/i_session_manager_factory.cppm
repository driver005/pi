export module pi.session.i_session_manager_factory;

import std;
export import pi.session.i_session_manager;
export import pi.types.result;
export import pi.types.session_manager_options;

/** Builds opened session managers; lets session discovery create them without knowing the class. */
export class ISessionManagerFactory {
public:
    virtual ~ISessionManagerFactory() = default;

    virtual Result<std::unique_ptr<ISessionManager>> create(const SessionManagerOptions& options) = 0;
};
