export module pi.types.sql_value;

import std;

/** A SQL value as the durable SQLite storage uses them: NULL (monostate), an integer or text. */
export using SqlValue = std::variant<std::monostate, std::int64_t, std::string>;
