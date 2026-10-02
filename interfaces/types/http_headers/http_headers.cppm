export module pi.types.http_headers;

import std;

/** Ordered header list; names compare case-insensitively when looked up. */
export using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
