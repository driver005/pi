export module pi.types.session_model_ref;

import std;

/** Provider and model id recorded in a session. */
export struct SessionModelRef {
    std::string provider;
    std::string modelId;
};
