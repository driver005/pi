module;

#include <cstdint>

export module pi.support.session_router;

import std;
export import pi.chord.i_service_endpoint;
export import pi.types.client_attachment;
export import pi.types.hosted_session;
export import pi.types.json;
export import pi.types.result;
export import pi.types.rpc_target;
export import pi.types.session_router_options;

/**
 * Routes each connected client to the session it is attached to: opens sessions on first use (one
 * opening at a time), keeps one attachment per client, runs a client's attach/detach/disconnect and
 * the start of its service calls in order, and drains calls in flight before releasing an
 * attachment. Clients are opaque numeric ids. Port of packages/server/src/session-router.ts; where
 * the TS awaits a promise this blocks the calling thread. Error codes: wrong ids and ends are
 * session_not_attached / server_draining, everything else comes from the host.
 */
export class SessionRouter {
public:
    explicit SessionRouter(SessionRouterOptions options)
        : m_options(std::move(options)) {}

    Result<std::optional<Json>> executeServiceCall(const Json& call, const RpcTarget& target, std::uint64_t client, const IServiceEndpoint::Publisher& publish, const ServiceContext& context) {
        const auto serial = clientLock(client);
        std::shared_ptr<ClientAttachment> attachment;
        {
            const std::lock_guard<std::mutex> ordered(*serial);
            auto required = requireAttachment(client, target);
            if (!required) {
                return std::unexpected(required.error());
            }
            attachment = *required;
            const std::lock_guard<std::mutex> state(attachment->mutex);
            if (attachment->releasing) {
                return std::unexpected(Error{"session_not_attached", "Session is not attached to this client"});
            }
            ++attachment->operations;
        }
        auto result = attachment->lease->invokeService(call, publish, context);
        {
            const std::lock_guard<std::mutex> state(attachment->mutex);
            --attachment->operations;
        }
        attachment->changed.notify_all();
        return result;
    }

    Result<void> attachClient(std::uint64_t client, const std::string& sessionId, const ServiceContext& context) {
        if (closing()) {
            return std::unexpected(drainingError());
        }
        const auto serial = clientLock(client);
        const std::lock_guard<std::mutex> ordered(*serial);
        return attachClientNow(client, sessionId, context);
    }

    Result<void> detachClient(std::uint64_t client, const ServiceContext& context) {
        const auto serial = clientLock(client);
        const std::lock_guard<std::mutex> ordered(*serial);
        if (const auto attachment = currentAttachment(client)) {
            return releaseAttachment(attachment, context, true);
        }
        return {};
    }

    /** Releases every attachment to the session and closes it. */
    Result<void> removeSession(const std::string& sessionId, const ServiceContext& context) {
        if (closing()) {
            return std::unexpected(drainingError());
        }
        const auto hosted = findHosted(sessionId);
        if (!hosted) {
            return {};
        }
        std::vector<std::shared_ptr<ClientAttachment>> attachments;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            attachments.assign(hosted->attachments.begin(), hosted->attachments.end());
        }
        std::vector<Error> errors;
        for (const auto& attachment : attachments) {
            if (auto released = releaseAttachment(attachment, context, true); !released) {
                errors.push_back(released.error());
            }
        }
        if (auto closed = hosted->handle->close(context); !closed) {
            errors.push_back(closed.error());
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_hosted.find(sessionId);
            if (found != m_hosted.end() && found->second == hosted) {
                m_hosted.erase(found);
            }
        }
        return combine(errors, "Failed to close Session " + sessionId);
    }

    /** The client's connection is gone: releases its attachment without telling it. */
    void disconnect(std::uint64_t client, const ServiceContext& context) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_disconnected.insert(client);
        }
        const auto serial = clientLock(client);
        {
            const std::lock_guard<std::mutex> ordered(*serial);
            if (const auto attachment = currentAttachment(client)) {
                releaseAttachment(attachment, context, false);
            }
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_disconnected.erase(client);
        m_clientLocks.erase(client);
    }

    /** Releases everything. Idempotent: later calls return the first outcome. */
    Result<void> close(const ServiceContext& context) {
        const std::lock_guard<std::mutex> once(m_closeMutex);
        if (!m_closeResult) {
            m_closeResult = closeNow(context);
        }
        return *m_closeResult;
    }

private:
    Result<void> attachClientNow(std::uint64_t client, const std::string& sessionId, const ServiceContext& context) {
        if (draining(client)) {
            return std::unexpected(drainingError());
        }
        const auto current = currentAttachment(client);
        if (current && current->sessionId == sessionId) {
            return {};
        }
        auto hosted = acquire(sessionId, context);
        if (!hosted) {
            return std::unexpected(hosted.error());
        }
        if (draining(client)) {
            return std::unexpected(drainingError());
        }
        if (current) {
            if (current->epoch == (*hosted)->epoch) {
                return {};
            }
            if (auto released = releaseAttachment(current, context, false); !released) {
                return released;
            }
        }
        auto attachment = std::make_shared<ClientAttachment>();
        attachment->id = m_options.ids->next();
        attachment->client = client;
        attachment->sessionId = (*hosted)->id;
        attachment->epoch = (*hosted)->epoch;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            (*hosted)->attachments.insert(attachment);
        }
        auto lease = (*hosted)->handle->attachClient(context);
        if (!lease) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            (*hosted)->attachments.erase(attachment);
            return std::unexpected(lease.error());
        }
        attachment->lease = std::move(*lease);
        bool valid = !closing();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_hosted.find((*hosted)->id);
            valid = valid && found != m_hosted.end() && found->second == *hosted &&
                    (*hosted)->attachments.contains(attachment) && !m_disconnected.contains(client);
            if (valid) {
                m_byClient[client] = attachment;
            }
        }
        if (!valid) {
            releaseAttachment(attachment, context, true);
            return std::unexpected(drainingError());
        }
        if (m_options.publishAttachment) {
            m_options.publishAttachment(client, SessionAttachment{attachment->sessionId, attachment->id}, context);
        }
        return {};
    }

    Result<std::shared_ptr<HostedSession>> acquire(const std::string& sessionId, const ServiceContext& context) {
        if (auto existing = findHosted(sessionId)) {
            return existing;
        }
        const std::lock_guard<std::mutex> opening(m_acquireMutex);
        if (auto existing = findHosted(sessionId)) {
            return existing;
        }
        return open(sessionId, context);
    }

    Result<std::shared_ptr<HostedSession>> open(const std::string& sessionId, const ServiceContext& context) {
        auto resolved = m_options.host->resolveSession(sessionId, context);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (auto existing = findHosted(*resolved)) {
            return existing;
        }
        auto handle = m_options.host->openSession(*resolved, context);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        if (closing()) {
            if (auto closed = (*handle)->close(context); !closed) {
                report(closed.error());
            }
            return std::unexpected(drainingError());
        }
        auto hosted = std::make_shared<HostedSession>();
        hosted->id = *resolved;
        hosted->handle = *handle;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            hosted->epoch = m_nextEpoch++;
            m_hosted[hosted->id] = hosted;
        }
        const std::weak_ptr<HostedSession> weak = hosted;
        hosted->handle->onTermination([this, weak](const std::optional<Error>& failure) {
            if (auto strong = weak.lock()) {
                invalidate(strong, failure);
            }
        });
        return hosted;
    }

    Result<std::shared_ptr<ClientAttachment>> requireAttachment(std::uint64_t client, const RpcTarget& target) {
        if (draining(client)) {
            return std::unexpected(drainingError());
        }
        const Error notAttached{"session_not_attached", "Session is not attached to this client"};
        if (!target.sessionId || !target.attachmentId) {
            return std::unexpected(notAttached);
        }
        auto attachment = currentAttachment(client);
        if (!attachment || attachment->sessionId != *target.sessionId || attachment->id != *target.attachmentId) {
            return std::unexpected(notAttached);
        }
        return attachment;
    }

    Result<void> releaseAttachment(const std::shared_ptr<ClientAttachment>& attachment, const ServiceContext& context, bool publish) {
        {
            std::unique_lock<std::mutex> state(attachment->mutex);
            if (attachment->releasing) {
                attachment->changed.wait(state, [&] { return attachment->released; });
                return {};
            }
            attachment->releasing = true;
            attachment->changed.wait(state, [&] { return attachment->operations == 0; });
        }
        if (attachment->lease) {
            attachment->lease->release(context);
        }
        clearAttachment(attachment, context, publish);
        {
            const std::lock_guard<std::mutex> state(attachment->mutex);
            attachment->released = true;
        }
        attachment->changed.notify_all();
        return {};
    }

    void clearAttachment(const std::shared_ptr<ClientAttachment>& attachment, const ServiceContext& context, bool publish) {
        bool wasCurrent = false;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto hosted = m_hosted.find(attachment->sessionId);
            if (hosted != m_hosted.end() && hosted->second->epoch == attachment->epoch) {
                hosted->second->attachments.erase(attachment);
            }
            const auto current = m_byClient.find(attachment->client);
            if (current != m_byClient.end() && current->second == attachment) {
                m_byClient.erase(current);
                wasCurrent = true;
            }
        }
        if (wasCurrent && publish && m_options.publishAttachment) {
            m_options.publishAttachment(attachment->client, std::nullopt, context);
        }
    }

    void invalidate(const std::shared_ptr<HostedSession>& hosted, const std::optional<Error>& failure) {
        auto work = [this, hosted, failure] {
            std::vector<std::shared_ptr<ClientAttachment>> attachments;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                const auto found = m_hosted.find(hosted->id);
                if (found == m_hosted.end() || found->second != hosted) {
                    return;
                }
                m_hosted.erase(found);
                attachments.assign(hosted->attachments.begin(), hosted->attachments.end());
            }
            for (const auto& attachment : attachments) {
                if (auto released = releaseAttachment(attachment, backgroundContext(), true); !released) {
                    report(released.error());
                }
            }
            if (failure) {
                report(*failure);
            }
        };
        if (m_options.executor) {
            m_options.executor->submit(std::move(work));
        } else {
            work();
        }
    }

    Result<void> closeNow(const ServiceContext& context) {
        std::vector<std::shared_ptr<std::mutex>> locks;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& entry : m_clientLocks) {
                locks.push_back(entry.second);
            }
        }
        for (const auto& serial : locks) {
            const std::lock_guard<std::mutex> ordered(*serial);
        }
        const std::lock_guard<std::mutex> opening(m_acquireMutex);
        std::vector<std::shared_ptr<HostedSession>> sessions;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& entry : m_hosted) {
                sessions.push_back(entry.second);
            }
        }
        std::vector<Error> errors;
        for (const auto& session : sessions) {
            std::vector<std::shared_ptr<ClientAttachment>> attachments;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                attachments.assign(session->attachments.begin(), session->attachments.end());
            }
            for (const auto& attachment : attachments) {
                if (auto released = releaseAttachment(attachment, context, true); !released) {
                    errors.push_back(released.error());
                }
            }
        }
        for (const auto& session : sessions) {
            auto closed = session->handle->close(context);
            if (!closed) {
                report(closed.error());
                errors.push_back(closed.error());
                continue;
            }
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_hosted.find(session->id);
            if (found != m_hosted.end() && found->second == session) {
                m_hosted.erase(found);
            }
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_byClient.clear();
        return combine(errors, "Failed to close routed Sessions");
    }

    std::shared_ptr<std::mutex> clientLock(std::uint64_t client) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto& entry = m_clientLocks[client];
        if (!entry) {
            entry = std::make_shared<std::mutex>();
        }
        return entry;
    }

    std::shared_ptr<HostedSession> findHosted(const std::string& sessionId) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_hosted.find(sessionId);
        return found == m_hosted.end() ? nullptr : found->second;
    }

    std::shared_ptr<ClientAttachment> currentAttachment(std::uint64_t client) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_byClient.find(client);
        return found == m_byClient.end() ? nullptr : found->second;
    }

    bool draining(std::uint64_t client) {
        if (closing()) {
            return true;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_disconnected.contains(client);
    }

    bool closing() const {
        return m_options.isClosing && m_options.isClosing();
    }

    ServiceContext backgroundContext() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    Error drainingError() const {
        return Error{"server_draining", "Server is draining"};
    }

    Result<void> combine(const std::vector<Error>& errors, const std::string& what) const {
        if (errors.empty()) {
            return {};
        }
        if (errors.size() == 1) {
            return std::unexpected(errors.front());
        }
        std::string message = what + ":";
        for (const Error& error : errors) {
            message += " " + error.message + ";";
        }
        return std::unexpected(Error{"internal_error", message});
    }

    void report(const Error& error) const {
        if (m_options.reportError) {
            m_options.reportError(error);
        }
    }

    SessionRouterOptions m_options;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<HostedSession>> m_hosted;
    std::map<std::uint64_t, std::shared_ptr<ClientAttachment>> m_byClient;
    std::set<std::uint64_t> m_disconnected;
    std::map<std::uint64_t, std::shared_ptr<std::mutex>> m_clientLocks;
    std::uint64_t m_nextEpoch = 1;
    std::mutex m_acquireMutex;
    std::mutex m_closeMutex;
    std::optional<Result<void>> m_closeResult;
};
