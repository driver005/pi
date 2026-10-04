export module pi.types.runtime_diagnostic;

import std;

/** A notice from setting up a session runtime. type: "info" | "warning" | "error". */
export struct RuntimeDiagnostic {
    std::string type;
    std::string message;
};
