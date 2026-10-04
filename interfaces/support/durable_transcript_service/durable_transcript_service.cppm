export module pi.support.durable_transcript_service;

import std;
export import pi.chord.i_remote_service;
export import pi.chord.i_replicated_state;

/**
 * The `pi.transcript` service over a durable conversation: its view as replicated state, `{conversation, entries, docs}`
 * (the active entries and the `pi.agent`, `pi.live`, `pi.inbox` and `pi.usage` documents). The service holds the mount it
 * serves, so the view stays maintained while the service lives. Counterpart of transcript-provider.ts.
 */
export class DurableTranscriptService : public IRemoteService {
public:
    explicit DurableTranscriptService(std::shared_ptr<IReplicatedState> view)
        : m_view(std::move(view)) {}

    std::map<std::string, Method> methods() override {
        return {};
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", m_view.get()}};
    }

private:
    std::shared_ptr<IReplicatedState> m_view;
};
