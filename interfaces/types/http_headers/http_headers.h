#pragma once

#include <string>
#include <utility>
#include <vector>

/** Ordered header list; names compare case-insensitively when looked up. */
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
