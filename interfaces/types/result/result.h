#pragma once

#include <expected>

#include "interfaces/types/error/error.h"

/** Success value or Error; the only error-handling channel (exceptions are disabled). */
template <typename T>
using Result = std::expected<T, Error>;
