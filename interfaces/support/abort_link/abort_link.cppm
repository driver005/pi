export module pi.support.abort_link;

import std;
export import pi.support.abort_signal;

/**
 * A signal that aborts when any of the signals it links aborts; the links are removed when it is destroyed.
 * Use `signal()` as the cancellation of one wait that several parties may cancel.
 */
export class AbortLink {
public:
    explicit AbortLink(const std::vector<const AbortSignal*>& sources) {
        for (const AbortSignal* source : sources) {
            if (source != nullptr) {
                AbortSignal* target = &m_signal;
                m_links.emplace_back(source, source->onAbort([target] { target->abort(); }));
            }
        }
    }

    AbortLink(const AbortLink&) = delete;
    AbortLink& operator=(const AbortLink&) = delete;

    ~AbortLink() {
        for (const auto& link : m_links) {
            link.first->removeListener(link.second);
        }
    }

    const AbortSignal* signal() const {
        return &m_signal;
    }

private:
    AbortSignal m_signal;
    std::vector<std::pair<const AbortSignal*, std::uint64_t>> m_links;
};
