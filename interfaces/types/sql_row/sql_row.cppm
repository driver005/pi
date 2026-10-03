export module pi.types.sql_row;

import std;
export import pi.types.sql_value;

/** One result row by column name. */
export using SqlRow = std::map<std::string, SqlValue>;
