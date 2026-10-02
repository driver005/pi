export module pi.platform.i_dynamic_libraries;

import std;
export import pi.types.result;

/** Loads shared libraries and looks up their symbols (dlopen/dlsym). Thread-safe. */
export class IDynamicLibraries {
public:
    virtual ~IDynamicLibraries() = default;

    /** Returns a handle for symbol() and close(); the error message is the loader's. */
    virtual Result<std::uint64_t> open(const std::string& path) = 0;
    /** The address of an exported symbol; the library stays loaded until close(). */
    virtual Result<void*> symbol(std::uint64_t library, const std::string& name) = 0;
    virtual void close(std::uint64_t library) = 0;
};
