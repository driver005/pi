module;

#include <nlohmann/json.hpp>

export module pi.server.i_service_attachment;

import std;
export import pi.chord.i_service_endpoint;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;

/**
 * One connection's endpoint of a hosted service set (the server-wide services, or one session's):
 * routes service calls, including the control calls, and is released when the connection detaches.
 */
export class IServiceAttachment {
public:
    virtual ~IServiceAttachment() = default;

    virtual Result<std::optional<Json>> invokeService(const Json& call, const IServiceEndpoint::Publisher& publish,
                                                      const ServiceContext& context) = 0;
    virtual void release(const ServiceContext& context) = 0;
};
