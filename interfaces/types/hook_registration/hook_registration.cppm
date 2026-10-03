export module pi.types.hook_registration;

import std;
export import pi.types.hook_handler;
export import pi.types.json;
export import pi.types.result;

/**
 * Handlers an extension supplies for the tasks named `task`, keyed by hook name. A handler takes a JSON
 * payload (its shape depends on the hook) and may return a JSON decision.
 */
export struct HookRegistration {
    std::string task;
    std::map<std::string, HookHandler> handlers;
};
