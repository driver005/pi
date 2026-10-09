export module pi.types.share_outcome;

import std;
export import pi.types.share_route;

/** Where a shared session ended up: the route taken, the link to open and, for a gist, the gist
 * behind the viewer link. */
export struct ShareOutcome {
    ShareRoute route = ShareRoute::Radius;
    std::string url;
    std::optional<std::string> gistUrl;
};
