export module pi.types.share_outcome;

import std;

/** Where a shared session ended up: `via` is "radius" or "gist"; `url` is the link to open and `gistUrl` the gist behind a viewer link. */
export struct ShareOutcome {
    std::string via;
    std::string url;
    std::optional<std::string> gistUrl;
};
