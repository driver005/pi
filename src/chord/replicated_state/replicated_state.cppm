module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.chord.replicated_state;

import std;
export import pi.chord.i_replicated_state;
export import pi.support.delta_differ;
export import pi.types.replicated_state_publication;
export import pi.types.result;

/**
 * A mutable JSON state that publishes the operations of every change: change() mutates a copy and
 * publishes the diff, replace() publishes a root replacement. Publications are numbered 1, 2, ...
 * (the initial value is sequence 0) and reach every listener in that order, also when changes are
 * made from several threads or from inside a listener. A change that alters nothing publishes
 * nothing. Counterpart of MutableReplicatedState in packages/chord/src/services/state.ts, which
 * derives the same operations from a draft proxy instead of a diff.
 */
export class ReplicatedState : public IReplicatedState {
public:
    using Mutator = std::function<void(Json& draft)>;

    explicit ReplicatedState(Json initial);

    ReplicatedStateSnapshot snapshot() const override;
    std::uint64_t subscribe(Listener listener) override;
    void unsubscribe(std::uint64_t id) override;

    Json value() const;
    /** Fails when called from inside a change callback of the same thread. */
    Result<void> change(const ServiceContext& context, const Mutator& mutate);
    Result<void> replace(const ServiceContext& context, Json value);

private:
    void commit(Json next, Json ops, const ServiceContext& context);
    void startDelivery();
    void deliver();

    DeltaDiffer m_differ;
    /** Serializes change(): read, mutate, diff and commit. */
    std::mutex m_changeMutex;
    std::atomic<std::thread::id> m_mutating{};
    mutable std::mutex m_mutex;
    Json m_value;
    std::int64_t m_sequence = 0;
    std::uint64_t m_nextListener = 1;
    std::map<std::uint64_t, Listener> m_listeners;
    std::deque<ReplicatedStatePublication> m_queue;
    bool m_delivering = false;
};

ReplicatedState::ReplicatedState(Json initial) : m_value(std::move(initial)) {}

ReplicatedStateSnapshot ReplicatedState::snapshot() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ReplicatedStateSnapshot out;
    out.value = m_value;
    out.sequence = m_sequence;
    return out;
}

Json ReplicatedState::value() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_value;
}

std::uint64_t ReplicatedState::subscribe(Listener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id = m_nextListener++;
    m_listeners.emplace(id, std::move(listener));
    return id;
}

void ReplicatedState::unsubscribe(std::uint64_t id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.erase(id);
}

void ReplicatedState::deliver() {
    while (true) {
        ReplicatedStatePublication publication;
        std::vector<Listener> listeners;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                m_delivering = false;
                return;
            }
            publication = std::move(m_queue.front());
            m_queue.pop_front();
            for (const auto& entry : m_listeners) {
                listeners.push_back(entry.second);
            }
        }
        for (const Listener& listener : listeners) {
            listener(publication.ops, publication.sequence, publication.context);
        }
    }
}

void ReplicatedState::commit(Json next, Json ops, const ServiceContext& context) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_value = std::move(next);
    ++m_sequence;
    ReplicatedStatePublication publication;
    publication.ops = std::move(ops);
    publication.sequence = m_sequence;
    publication.context = context;
    m_queue.push_back(std::move(publication));
}

void ReplicatedState::startDelivery() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_delivering) {
            return;
        }
        m_delivering = true;
    }
    deliver();
}

Result<void> ReplicatedState::change(const ServiceContext& context, const Mutator& mutate) {
    if (m_mutating.load() == std::this_thread::get_id()) {
        return std::unexpected(Error{"state", "Replicated state cannot be changed reentrantly from a change callback"});
    }
    {
        const std::lock_guard<std::mutex> change(m_changeMutex);
        const Json current = value();
        Json next = current;
        m_mutating.store(std::this_thread::get_id());
        mutate(next);
        m_mutating.store(std::thread::id{});
        Json ops = m_differ.diff(current, next);
        if (ops.empty()) {
            return {};
        }
        commit(std::move(next), std::move(ops), context);
    }
    startDelivery();
    return {};
}

Result<void> ReplicatedState::replace(const ServiceContext& context, Json next) {
    if (m_mutating.load() == std::this_thread::get_id()) {
        return std::unexpected(Error{"state", "Replicated state cannot be replaced from a change callback"});
    }
    {
        const std::lock_guard<std::mutex> change(m_changeMutex);
        if (value() == next) {
            return {};
        }
        Json ops = Json::array({Json::array({"r", next})});
        commit(std::move(next), std::move(ops), context);
    }
    startDelivery();
    return {};
}
