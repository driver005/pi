module;

#include <nlohmann/json.hpp>

export module pi.chord.i_remote_service;

import std;
export import pi.chord.i_replicated_state;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;

/**
 * One service implementation as the provider sees it: named methods (JSON arguments in, an optional
 * JSON result out) and named replicated states. Method failures are Errors; codes of the
 * RemoteServiceError family and the server's own codes cross the protocol boundary as they are,
 * anything else is reported as an internal error.
 */
export class IRemoteService {
public:
    using Method = std::function<Result<std::optional<Json>>(const std::vector<Json>& args, const ServiceContext& context)>;

    virtual ~IRemoteService() = default;

    /** Methods by name. */
    virtual std::map<std::string, Method> methods() = 0;
    /** States by name; the pointers stay valid as long as the service lives. */
    virtual std::map<std::string, IReplicatedState*> states() = 0;
};
