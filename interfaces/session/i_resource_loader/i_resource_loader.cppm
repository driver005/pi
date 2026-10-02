export module pi.session.i_resource_loader;

import std;
export import pi.types.loaded_resources;
export import pi.types.result;

/** Discovers the resources of a session (context files, skills, prompt templates, SYSTEM.md). */
export class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;

    /** Resources found by the last load. */
    virtual LoadedResources resources() const = 0;
    /** Reads everything again (after settings or files changed). */
    virtual Result<void> reload() = 0;
};
