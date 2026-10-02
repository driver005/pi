export module pi.types.result;

import std;
export import pi.types.error;

/** Success value or Error; the only error-handling channel (exceptions are disabled). */
export template <typename T>
using Result = std::expected<T, Error>;
