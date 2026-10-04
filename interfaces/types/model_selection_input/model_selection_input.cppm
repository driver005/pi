export module pi.types.model_selection_input;

import std;
export import pi.types.model;
export import pi.types.session_model_ref;

/** What the starting model and thinking level are chosen from. */
export struct ModelSelectionInput {
    /** Models whose provider has usable credentials. */
    std::vector<Model> available;
    /** Every known model (a requested model need not have credentials yet). */
    std::vector<Model> all;
    /** Model and thinking level recorded in the session being restored. */
    std::optional<SessionModelRef> saved;
    std::optional<std::string> savedThinking;
    /** "provider/id" or a bare id, with an optional ":level" suffix. */
    std::optional<std::string> requestedModel;
    std::optional<std::string> requestedThinking;
};
