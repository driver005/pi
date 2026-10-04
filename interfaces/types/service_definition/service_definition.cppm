export module pi.types.service_definition;

import std;

/** One published service: its id and whether it has one instance ("singleton") or many ("keyed"). */
export struct ServiceDefinition {
    std::string id;
    std::string mode = "singleton";
};
