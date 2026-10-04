export module pi.support.virtual_model_names;

import std;
export import pi.types.model;

/** The names that mark virtual models: the api of their catalog entries and the type of the entries that store router state. */
export class VirtualModelNames {
public:
    /** Api of a virtual catalog entry; a request for it fails unless it was routed to a physical model first. */
    std::string api() const {
        return "pi-virtual";
    }

    /** Type of the session entry that stores router state on a branch; its data is `{provider, modelId, state}`. */
    std::string stateEntryType() const {
        return "pi.virtual-model-state";
    }

    bool isVirtual(const Model& model) const {
        return model.api == api();
    }

    bool isVirtualApi(const std::string& modelApi) const {
        return modelApi == api();
    }
};
