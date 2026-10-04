export module pi.types.prepared_request;

import std;
export import pi.types.model;
export import pi.types.stream_options;

/** A model and stream options with credentials, headers and base URL already resolved for one provider request. */
export struct PreparedRequest {
    Model model;
    StreamOptions options;
};
