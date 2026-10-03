export module pi.types.serve_dependencies;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_crypto;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.platform.i_logger;
export import pi.provider.i_model_runtime;
export import pi.session.i_session_runtime_factory;
export import pi.session.i_session_store;

/** The services a protocol server runs on; the composition root owns them and they must outlive it. */
export struct ServeDependencies {
    IFileSystem* files = nullptr;
    const IClock* clock = nullptr;
    IIdGenerator* ids = nullptr;
    const ICrypto* crypto = nullptr;
    ILogger* logger = nullptr;
    ISessionStore* sessions = nullptr;
    ISessionRuntimeFactory* runtimes = nullptr;
    IModelRuntime* models = nullptr;
};
