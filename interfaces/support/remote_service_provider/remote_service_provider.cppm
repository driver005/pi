module;

#include <cstdint>

export module pi.support.remote_service_provider;

import std;
export import pi.chord.i_remote_service;
export import pi.chord.i_service_provider;
export import pi.support.service_wire;
export import pi.types.provider_instance;
export import pi.types.provider_subscriber;
export import pi.types.service_definition;
export import pi.types.service_registration;

/**
 * Hosts a fixed catalogue of services. Singletons are provided, withdrawn and replaced; keyed
 * services spawn instances addressed by (key, generation). Remote consumers invoke methods and
 * subscribe to the replicated states of a service: a subscription takes an atomic snapshot, then
 * receives every later update in order (state ops, replacement, spawn, close, unavailable), with a
 * full reset when more than 100 updates pile up before activation or behind a slow listener.
 * Errors carry the RemoteServiceError codes (service_not_allowed, service_not_found,
 * service_mode_mismatch, service_member_not_found, service_member_mismatch,
 * service_instance_not_found, service_stale_instance). Port of RemoteServiceProvider in
 * packages/chord/src/services/provider.ts.
 */
export class RemoteServiceProvider : public IServiceProvider {
public:
    static constexpr std::size_t kMaxBufferedUpdates = 100;

    explicit RemoteServiceProvider(const std::vector<ServiceDefinition>& entries);
    ~RemoteServiceProvider() override;

    Result<void> provide(const std::string& serviceId, std::shared_ptr<IRemoteService> implementation);
    /** Disconnects a singleton; subscriptions stay and hear `unavailable`. */
    Result<void> withdraw(const std::string& serviceId);
    /** Swaps a singleton for one with the same member shape; subscriptions hear `replaced`. */
    Result<void> replace(const std::string& serviceId, std::shared_ptr<IRemoteService> implementation);
    /** Adds a keyed instance; the returned function removes it again. */
    Result<std::function<void()>> spawn(const std::string& serviceId, const std::string& key,
                                        std::shared_ptr<IRemoteService> implementation);
    void dispose();

    Json catalogue() const override;
    Result<std::optional<Json>> invoke(const Json& call, const ServiceContext& context) override;
    Result<ServiceSubscription> subscribe(const std::string& serviceId, const std::string& mode,
                                          UpdateListener listener) override;

private:
    Result<ServiceRegistration*> registration(const std::string& serviceId, const std::string& mode);
    Result<void> allowed(const std::string& serviceId) const;
    Result<std::shared_ptr<ProviderInstance>> classify(const std::string& serviceId,
                                                       std::shared_ptr<IRemoteService> implementation,
                                                       const Json& address) const;
    void wire(ServiceRegistration& registration, const std::shared_ptr<ProviderInstance>& instance);
    void unwire(ProviderInstance& instance);
    Result<std::shared_ptr<ProviderInstance>> resolve(const ServiceRegistration& registration, const Json& address) const;
    std::map<std::string, std::string> shape(const ProviderInstance& instance) const;
    Json snapshotInstance(const ProviderInstance& instance) const;
    Json snapshot(const ServiceRegistration& registration) const;
    void recordSequences(std::map<std::string, std::int64_t>& sequences, const Json& instances) const;
    bool covered(std::map<std::string, std::int64_t>& sequences, const Json& update) const;
    std::string memberKey(const Json& instance, const std::string& member) const;
    void emit(ServiceRegistration& registration, const Json& update, const ServiceContext& context);
    void drain(const std::shared_ptr<ProviderSubscriber>& subscriber) const;
    Error failure(const std::string& code, const std::string& message) const;

    mutable std::mutex m_mutex;
    std::vector<Json> m_catalogue;
    std::map<std::string, std::unique_ptr<ServiceRegistration>> m_registrations;
    bool m_disposed = false;
};

Error RemoteServiceProvider::failure(const std::string& code, const std::string& message) const {
    return Error{code, message};
}

RemoteServiceProvider::RemoteServiceProvider(const std::vector<ServiceDefinition>& entries) {
    for (const ServiceDefinition& entry : entries) {
        if (m_registrations.contains(entry.id)) {
            continue;
        }
        m_catalogue.push_back(Json{{"serviceId", entry.id}, {"mode", entry.mode}});
        auto registration = std::make_unique<ServiceRegistration>();
        registration->serviceId = entry.id;
        registration->mode = entry.mode;
        m_registrations.emplace(entry.id, std::move(registration));
    }
}

RemoteServiceProvider::~RemoteServiceProvider() {
    dispose();
}

Json RemoteServiceProvider::catalogue() const {
    return Json(m_catalogue);
}

Result<void> RemoteServiceProvider::allowed(const std::string& serviceId) const {
    if (!m_registrations.contains(serviceId)) {
        return std::unexpected(failure("service_not_allowed", "Remote service " + serviceId + " is not allowlisted"));
    }
    return {};
}

Result<ServiceRegistration*> RemoteServiceProvider::registration(const std::string& serviceId, const std::string& mode) {
    const auto found = m_registrations.find(serviceId);
    if (found == m_registrations.end()) {
        return std::unexpected(failure("service_not_found", "Unknown remote service " + serviceId));
    }
    if (found->second->mode != mode) {
        return std::unexpected(failure("service_mode_mismatch", "Remote service " + serviceId + " is " +
                                                                    found->second->mode + ", not " + mode));
    }
    return found->second.get();
}

Result<std::shared_ptr<ProviderInstance>> RemoteServiceProvider::classify(const std::string& serviceId,
                                                                          std::shared_ptr<IRemoteService> implementation,
                                                                          const Json& address) const {
    if (implementation == nullptr) {
        return std::unexpected(failure("service_invalid_value", "Remote service " + serviceId + " implementation must be an object"));
    }
    auto instance = std::make_shared<ProviderInstance>();
    instance->address = address;
    instance->implementation = implementation;
    instance->methods = implementation->methods();
    instance->states = implementation->states();
    for (const auto& method : instance->methods) {
        if (instance->states.contains(method.first)) {
            return std::unexpected(failure("service_invalid_value", "Remote service member " + serviceId + "." + method.first + " is both a method and a state"));
        }
    }
    if (instance->methods.empty() && instance->states.empty()) {
        return std::unexpected(failure("service_invalid_value", "Remote service " + serviceId + " has no members"));
    }
    return instance;
}

std::map<std::string, std::string> RemoteServiceProvider::shape(const ProviderInstance& instance) const {
    std::map<std::string, std::string> out;
    for (const auto& method : instance.methods) {
        out[method.first] = "method";
    }
    for (const auto& state : instance.states) {
        out[state.first] = "state";
    }
    return out;
}

Json RemoteServiceProvider::snapshotInstance(const ProviderInstance& instance) const {
    std::map<std::string, Json> members;
    for (const auto& method : instance.methods) {
        members[method.first] = Json{{"name", method.first}, {"kind", "method"}};
    }
    for (const auto& state : instance.states) {
        const ReplicatedStateSnapshot current = state.second->snapshot();
        members[state.first] = Json{{"name", state.first},
                                    {"kind", "state"},
                                    {"sequence", current.sequence},
                                    {"ops", Json::array({Json::array({"r", current.value})})}};
    }
    Json list = Json::array();
    for (auto& member : members) {
        list.push_back(std::move(member.second));
    }
    Json out = Json::object();
    if (!instance.address.is_null()) {
        out["instance"] = instance.address;
    }
    out["members"] = std::move(list);
    return out;
}

Json RemoteServiceProvider::snapshot(const ServiceRegistration& registration) const {
    Json instances = Json::array();
    if (registration.mode == "singleton") {
        if (registration.singleton) {
            instances.push_back(snapshotInstance(*registration.singleton));
        }
    } else {
        for (const auto& entry : registration.instances) {
            instances.push_back(snapshotInstance(*entry.second));
        }
    }
    return Json{{"serviceId", registration.serviceId}, {"mode", registration.mode}, {"instances", std::move(instances)}};
}

std::string RemoteServiceProvider::memberKey(const Json& instance, const std::string& member) const {
    if (instance.is_object()) {
        return Json::array({instance["key"], instance["generation"], member}).dump();
    }
    return Json::array({member}).dump();
}

void RemoteServiceProvider::recordSequences(std::map<std::string, std::int64_t>& sequences, const Json& instances) const {
    for (const Json& instance : instances) {
        const Json address = instance.contains("instance") ? instance["instance"] : Json();
        for (const Json& member : instance["members"]) {
            if (member["kind"] == "state") {
                sequences[memberKey(address, member["name"].get<std::string>())] = member["sequence"].get<std::int64_t>();
            }
        }
    }
}

bool RemoteServiceProvider::covered(std::map<std::string, std::int64_t>& sequences, const Json& update) const {
    const std::string type = update["type"].get<std::string>();
    if (type == "state") {
        const Json address = update.contains("instance") ? update["instance"] : Json();
        const std::string key = memberKey(address, update["member"].get<std::string>());
        const auto found = sequences.find(key);
        if (found == sequences.end()) {
            return false;
        }
        if (update["sequence"].get<std::int64_t>() <= found->second) {
            return true;
        }
        sequences.erase(found);
        return false;
    }
    if (type == "reset") {
        sequences.clear();
        recordSequences(sequences, update["snapshot"]["instances"]);
    } else if (type == "replaced") {
        sequences.clear();
        recordSequences(sequences, Json::array({update["snapshot"]}));
    } else if (type == "spawned") {
        recordSequences(sequences, Json::array({update["instance"]}));
    } else if (type == "unavailable") {
        sequences.clear();
    } else if (type == "closed") {
        const std::string prefix = Json::array({update["instance"]["key"], update["instance"]["generation"]}).dump();
        const std::string head = prefix.substr(0, prefix.size() - 1) + ",";
        for (auto it = sequences.begin(); it != sequences.end();) {
            it = it->first.starts_with(head) ? sequences.erase(it) : std::next(it);
        }
    }
    return false;
}

void RemoteServiceProvider::drain(const std::shared_ptr<ProviderSubscriber>& subscriber) const {
    {
        const std::lock_guard<std::mutex> lock(subscriber->mutex);
        if (!subscriber->active || subscriber->closed || subscriber->draining) {
            return;
        }
        subscriber->draining = true;
    }
    while (true) {
        std::pair<Json, ServiceContext> entry;
        {
            const std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (subscriber->closed || subscriber->buffer.empty()) {
                subscriber->draining = false;
                if (subscriber->terminated) {
                    subscriber->closed = true;
                }
                return;
            }
            entry = std::move(subscriber->buffer.front());
            subscriber->buffer.pop_front();
        }
        subscriber->listener(entry.first, entry.second);
    }
}

void RemoteServiceProvider::emit(ServiceRegistration& registration, const Json& update, const ServiceContext& context) {
    std::vector<std::shared_ptr<ProviderSubscriber>> subscribers;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        subscribers = registration.subscribers;
        Json fullReset;
        for (const auto& subscriber : subscribers) {
            const std::lock_guard<std::mutex> state(subscriber->mutex);
            if (subscriber->closed || covered(subscriber->snapshotSequences, update)) {
                continue;
            }
            if (subscriber->buffer.size() == kMaxBufferedUpdates) {
                if (fullReset.is_null()) {
                    fullReset = Json{{"type", "reset"}, {"snapshot", snapshot(registration)}};
                }
                subscriber->buffer.clear();
                subscriber->snapshotSequences.clear();
                recordSequences(subscriber->snapshotSequences, fullReset["snapshot"]["instances"]);
                subscriber->buffer.emplace_back(fullReset, context);
            } else {
                subscriber->buffer.emplace_back(update, context);
            }
        }
    }
    for (const auto& subscriber : subscribers) {
        drain(subscriber);
    }
}

void RemoteServiceProvider::wire(ServiceRegistration& registration, const std::shared_ptr<ProviderInstance>& instance) {
    ServiceRegistration* target = &registration;
    for (const auto& entry : instance->states) {
        const std::string name = entry.first;
        const std::uint64_t id = entry.second->subscribe(
            [this, target, instance, name](const Json& ops, std::int64_t sequence, const ServiceContext& context) {
                if (!instance->active.load()) {
                    return;
                }
                Json update{{"type", "state"}, {"member", name}, {"sequence", sequence}, {"ops", ops}};
                if (!instance->address.is_null()) {
                    update["instance"] = instance->address;
                }
                emit(*target, update, context);
            });
        instance->stateListeners.emplace_back(entry.second, id);
    }
}

void RemoteServiceProvider::unwire(ProviderInstance& instance) {
    instance.active.store(false);
    for (const auto& listener : instance.stateListeners) {
        listener.first->unsubscribe(listener.second);
    }
    instance.stateListeners.clear();
}

Result<void> RemoteServiceProvider::provide(const std::string& serviceId, std::shared_ptr<IRemoteService> implementation) {
    ServiceRegistration* target = nullptr;
    std::shared_ptr<ProviderInstance> instance;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return ok;
        }
        auto found = registration(serviceId, "singleton");
        if (!found) {
            return std::unexpected(found.error());
        }
        target = *found;
        if (target->singleton) {
            return std::unexpected(failure("service_mode_mismatch", "Remote service " + serviceId + " already has a provider"));
        }
        auto classified = classify(serviceId, std::move(implementation), Json());
        if (!classified) {
            return std::unexpected(classified.error());
        }
        instance = *classified;
        const auto newShape = shape(*instance);
        if (target->singletonShape && *target->singletonShape != newShape) {
            return std::unexpected(failure("service_member_mismatch", "Remote service " + serviceId + " replacement must preserve its member shape"));
        }
        target->singleton = instance;
        target->singletonShape = newShape;
    }
    wire(*target, instance);
    return {};
}

Result<void> RemoteServiceProvider::withdraw(const std::string& serviceId) {
    ServiceRegistration* target = nullptr;
    std::shared_ptr<ProviderInstance> previous;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return ok;
        }
        auto found = registration(serviceId, "singleton");
        if (!found) {
            return std::unexpected(found.error());
        }
        target = *found;
        previous = std::move(target->singleton);
        target->singleton.reset();
    }
    if (!previous) {
        return {};
    }
    unwire(*previous);
    emit(*target, Json{{"type", "unavailable"}}, ServiceContext{});
    return {};
}

Result<void> RemoteServiceProvider::replace(const std::string& serviceId, std::shared_ptr<IRemoteService> implementation) {
    ServiceRegistration* target = nullptr;
    std::shared_ptr<ProviderInstance> previous;
    std::shared_ptr<ProviderInstance> replacement;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return ok;
        }
        auto found = registration(serviceId, "singleton");
        if (!found) {
            return std::unexpected(found.error());
        }
        target = *found;
        auto classified = classify(serviceId, std::move(implementation), Json());
        if (!classified) {
            return std::unexpected(classified.error());
        }
        replacement = *classified;
        const auto newShape = shape(*replacement);
        if (target->singletonShape && *target->singletonShape != newShape) {
            return std::unexpected(failure("service_member_mismatch", "Remote service " + serviceId + " replacement must preserve its member shape"));
        }
        previous = target->singleton;
        target->singleton = replacement;
        target->singletonShape = newShape;
    }
    if (previous) {
        unwire(*previous);
    }
    wire(*target, replacement);
    emit(*target, Json{{"type", "replaced"}, {"snapshot", snapshotInstance(*replacement)}}, ServiceContext{});
    return {};
}

Result<std::function<void()>> RemoteServiceProvider::spawn(const std::string& serviceId, const std::string& key,
                                                           std::shared_ptr<IRemoteService> implementation) {
    if (key.empty()) {
        return std::unexpected(failure("service_invalid_value", "Remote service instance key must not be empty"));
    }
    ServiceRegistration* target = nullptr;
    std::shared_ptr<ProviderInstance> instance;
    Json address;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return std::unexpected(ok.error());
        }
        auto found = registration(serviceId, "keyed");
        if (!found) {
            return std::unexpected(found.error());
        }
        target = *found;
        if (target->instances.contains(key)) {
            return std::unexpected(failure("service_mode_mismatch", "Remote service " + serviceId +
                                                                        " already has a live instance with key " + key));
        }
        const std::int64_t generation = ++target->generations[key];
        address = Json{{"key", key}, {"generation", generation}};
        auto classified = classify(serviceId, std::move(implementation), address);
        if (!classified) {
            return std::unexpected(classified.error());
        }
        instance = *classified;
        target->instances[key] = instance;
    }
    wire(*target, instance);
    emit(*target, Json{{"type", "spawned"}, {"instance", snapshotInstance(*instance)}}, ServiceContext{});
    auto closed = std::make_shared<std::atomic<bool>>(false);
    return std::function<void()>([this, target, instance, key, address, closed]() {
        if (closed->exchange(true)) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = target->instances.find(key);
            if (found == target->instances.end() || found->second != instance) {
                return;
            }
            target->instances.erase(found);
        }
        unwire(*instance);
        emit(*target, Json{{"type", "closed"}, {"instance", address}}, ServiceContext{});
    });
}

Result<std::shared_ptr<ProviderInstance>> RemoteServiceProvider::resolve(const ServiceRegistration& registration,
                                                                         const Json& address) const {
    if (registration.mode == "singleton") {
        if (!address.is_null()) {
            return std::unexpected(failure("service_mode_mismatch", "Remote service " + registration.serviceId + " is singleton"));
        }
        if (!registration.singleton) {
            return std::unexpected(failure("service_not_found", "Remote service " + registration.serviceId + " has no provider"));
        }
        return registration.singleton;
    }
    if (address.is_null()) {
        return std::unexpected(failure("service_mode_mismatch", "Remote service " + registration.serviceId + " is keyed"));
    }
    const std::string key = address["key"].get<std::string>();
    const auto found = registration.instances.find(key);
    if (found == registration.instances.end()) {
        return std::unexpected(failure("service_instance_not_found", "Remote service " + registration.serviceId + " has no instance " + key));
    }
    if (found->second->address["generation"] != address["generation"]) {
        return std::unexpected(failure("service_stale_instance", "Remote service " + registration.serviceId + " instance " + key + " is stale"));
    }
    return found->second;
}

Result<std::optional<Json>> RemoteServiceProvider::invoke(const Json& call, const ServiceContext& context) {
    const std::string serviceId = call["serviceId"].get<std::string>();
    const std::string memberName = call["member"].get<std::string>();
    std::shared_ptr<ProviderInstance> instance;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return std::unexpected(ok.error());
        }
        const ServiceRegistration& entry = *m_registrations.at(serviceId);
        auto resolved = resolve(entry, call.contains("instance") ? call["instance"] : Json());
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        instance = *resolved;
    }
    const auto method = instance->methods.find(memberName);
    if (method == instance->methods.end()) {
        if (instance->states.contains(memberName)) {
            return std::unexpected(failure("service_member_mismatch", "Remote service member " + serviceId + "." + memberName + " is not a method"));
        }
        return std::unexpected(failure("service_member_not_found", "Unknown remote service member " + serviceId + "." + memberName));
    }
    return method->second(call["args"].get<std::vector<Json>>(), context);
}

Result<ServiceSubscription> RemoteServiceProvider::subscribe(const std::string& serviceId, const std::string& mode,
                                                             UpdateListener listener) {
    auto subscriber = std::make_shared<ProviderSubscriber>();
    subscriber->listener = std::move(listener);
    ServiceRegistration* target = nullptr;
    ServiceSubscription out;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return std::unexpected(failure("disposed", "Remote service provider is disposed"));
        }
        if (auto ok = allowed(serviceId); !ok) {
            return std::unexpected(ok.error());
        }
        auto found = registration(serviceId, mode);
        if (!found) {
            return std::unexpected(found.error());
        }
        target = *found;
        if (target->mode == "singleton" && !target->singleton) {
            return std::unexpected(failure("service_not_found", "Remote service " + serviceId + " has no provider"));
        }
        target->subscribers.push_back(subscriber);
        out.snapshot = snapshot(*target);
        recordSequences(subscriber->snapshotSequences, out.snapshot["instances"]);
    }
    out.activate = [this, subscriber]() {
        {
            const std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (subscriber->closed || subscriber->active) {
                return;
            }
            subscriber->active = true;
        }
        drain(subscriber);
    };
    out.close = [this, target, subscriber]() {
        {
            const std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (subscriber->closed) {
                return;
            }
            subscriber->closed = true;
            subscriber->buffer.clear();
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::erase(target->subscribers, subscriber);
    };
    return out;
}

void RemoteServiceProvider::dispose() {
    std::vector<std::pair<ServiceRegistration*, std::vector<std::shared_ptr<ProviderInstance>>>> instances;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disposed) {
            return;
        }
        m_disposed = true;
        for (auto& entry : m_registrations) {
            ServiceRegistration& registration = *entry.second;
            std::vector<std::shared_ptr<ProviderInstance>> gone;
            if (registration.singleton) {
                gone.push_back(std::move(registration.singleton));
                registration.singleton.reset();
            }
            for (auto& instance : registration.instances) {
                gone.push_back(instance.second);
            }
            registration.instances.clear();
            instances.emplace_back(&registration, std::move(gone));
        }
    }
    for (auto& entry : instances) {
        for (const auto& instance : entry.second) {
            unwire(*instance);
        }
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_registrations) {
        for (const auto& subscriber : entry.second->subscribers) {
            const std::lock_guard<std::mutex> state(subscriber->mutex);
            if (subscriber->active && !subscriber->draining) {
                subscriber->closed = true;
                subscriber->buffer.clear();
            } else {
                subscriber->terminated = true;
            }
        }
        entry.second->subscribers.clear();
    }
}
