export module pi.testing.fake_session_catalog;

import std;
export import pi.server.i_session_catalog;

/** In-memory ISessionCatalog: ids are "s1", "s2", ... unless given; creation time is the call count. */
export class FakeSessionCatalog : public ISessionCatalog {
public:
    Result<std::vector<SessionRecord>> list() override;
    Result<SessionRecord> create(const std::optional<std::string>& id) override;
    Result<void> remove(const std::string& id) override;
    Result<SessionRecord> resolve(const std::string& idOrPrefix) override;

    /** Makes every later call fail with this error until cleared with std::nullopt. */
    void fail(const std::optional<Error>& error);
    int removed() const;

private:
    mutable std::mutex m_mutex;
    std::vector<SessionRecord> m_records;
    std::optional<Error> m_failure;
    int m_counter = 0;
    int m_removed = 0;
};

Result<std::vector<SessionRecord>> FakeSessionCatalog::list() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure) {
        return std::unexpected(*m_failure);
    }
    std::vector<SessionRecord> newestFirst(m_records.rbegin(), m_records.rend());
    return newestFirst;
}

Result<SessionRecord> FakeSessionCatalog::create(const std::optional<std::string>& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure) {
        return std::unexpected(*m_failure);
    }
    ++m_counter;
    SessionRecord record;
    record.id = id ? *id : "s" + std::to_string(m_counter);
    record.createdAt = 1000 + m_counter;
    record.cwd = "/work";
    record.directory = "/sessions/" + record.id;
    for (const SessionRecord& existing : m_records) {
        if (existing.id == record.id) {
            return std::unexpected(Error{"session_exists", "Session " + record.id + " already exists"});
        }
    }
    m_records.push_back(record);
    return record;
}

Result<void> FakeSessionCatalog::remove(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure) {
        return std::unexpected(*m_failure);
    }
    const auto found = std::ranges::find(m_records, id, &SessionRecord::id);
    if (found == m_records.end()) {
        return std::unexpected(Error{"session_not_found", "Session was not found"});
    }
    m_records.erase(found);
    ++m_removed;
    return {};
}

Result<SessionRecord> FakeSessionCatalog::resolve(const std::string& idOrPrefix) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure) {
        return std::unexpected(*m_failure);
    }
    std::vector<SessionRecord> matches;
    for (const SessionRecord& record : m_records) {
        if (record.id == idOrPrefix) {
            return record;
        }
        if (!idOrPrefix.empty() && record.id.starts_with(idOrPrefix)) {
            matches.push_back(record);
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.size() > 1) {
        return std::unexpected(Error{"session_ambiguous", "Session ID matches more than one session"});
    }
    return std::unexpected(Error{"session_not_found", "Session was not found"});
}

void FakeSessionCatalog::fail(const std::optional<Error>& error) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_failure = error;
}

int FakeSessionCatalog::removed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_removed;
}
