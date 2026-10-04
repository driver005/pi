export module pi.support.delta_decoder;

import std;
export import pi.support.delta_validator;
export import pi.types.json;
export import pi.types.result;

/**
 * Inverse of DeltaEncoder for one state stream: resolves path ids and short forms back into decoded
 * ops (`["s", path, value]`). Must see every batch its encoder produced, starting with the base.
 * Error codes: "delta_op", "delta_unsafe_path", "delta_path". Port of decoder() in
 * packages/chord/src/delta/index.ts.
 */
export class DeltaDecoder {
public:
    Result<Json> decode(const Json& wire) {
        if (!wire.is_array()) {
            return std::unexpected(Error{"delta_op", "ops is not an array"});
        }
        std::optional<Json> previous;
        Json out = Json::array();
        for (const Json& op : wire) {
            if (auto valid = m_validator.validateWireOp(op); !valid) {
                return std::unexpected(valid.error());
            }
            if (op[0] == "#") {
                m_paths[static_cast<std::int64_t>(op[1].get<double>())] = op[2];
                continue;
            }
            if (op[0] == "r") {
                out.push_back(op);
                m_paths.clear();
                previous.reset();
                continue;
            }
            const bool shortened = shortForm(op);
            auto path = resolvePath(op, shortened, previous);
            if (!path) {
                return std::unexpected(path.error());
            }
            if (!shortened) {
                previous = *path;
            }
            if (auto safe = m_validator.validatePath(*path); !safe) {
                return std::unexpected(safe.error());
            }
            if (op[0] != "p" && op[0] != "m" && path->empty()) {
                return std::unexpected(Error{"delta_path", "unresolvable path: []"});
            }
            if (op[0] == "m") {
                out.push_back(Json::array({"m", *path, op[shortened ? 1 : 2]}));
                continue;
            }
            out.push_back(expand(op, *path, shortened));
        }
        return out;
    }

private:
    bool shortForm(const Json& op) const {
        const std::string verb = op[0].get<std::string>();
        if (verb == "d") {
            return op.size() == 1;
        }
        if (verb == "p") {
            return op.size() == 4;
        }
        return op.size() == 2;
    }

    Result<Json> resolvePath(const Json& op, bool shortened, std::optional<Json>& previous) const {
        if (shortened) {
            if (!previous) {
                return std::unexpected(Error{"delta_path", "unresolvable path: []"});
            }
            return *previous;
        }
        const Json& reference = op[1];
        if (reference.is_number()) {
            const auto found = m_paths.find(static_cast<std::int64_t>(reference.get<double>()));
            if (found == m_paths.end()) {
                return std::unexpected(Error{"delta_path", "unresolvable path: " + reference.dump()});
            }
            return found->second;
        }
        return reference;
    }

    Json expand(const Json& op, const Json& path, bool shortened) const {
        const std::string verb = op[0].get<std::string>();
        const std::size_t offset = shortened ? 0 : 1;
        if (verb == "d") {
            return Json::array({"d", path});
        }
        if (verb == "p") {
            return Json::array({"p", path, op[1 + offset], op[2 + offset], op[3 + offset]});
        }
        return Json::array({verb, path, op[1 + offset]});
    }

    DeltaValidator m_validator;
    std::map<std::int64_t, Json> m_paths;
};
