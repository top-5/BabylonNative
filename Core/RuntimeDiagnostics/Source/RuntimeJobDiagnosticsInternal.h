#pragma once
#include <Babylon/RuntimeJobDiagnostics.h>
#include <array>
#include <cmath>
#include <limits>

namespace Babylon::RuntimeDiagnostics::Detail
{
    // Shared by noexcept wrapper catches; atomically marks missing observations.
    // Exposed internally so helper tests can verify fail-closed snapshots.
    void RecordObserverFailure() noexcept;
    struct StartInfo { bool accepted{}; uint64_t queuedAt{}, startedAt{}; };
    class Tracker final
    {
    public:
        static constexpr size_t Capacity = 1024;
        explicit Tracker(bool available = true, uint64_t nextId = 1) noexcept : m_nextId{nextId}
        { m_state.available = available; }
        void Configure(bool enabled) noexcept { m_state.enabled = enabled && m_state.available; }
        bool Enabled() const noexcept { return m_state.enabled; }
        Token Enqueue(uint64_t parent, uint64_t now) noexcept
        {
            if (!Enabled()) return {};
            Increment(m_state.enqueued);
            if (m_outstanding == Capacity || m_nextId == 0)
            { Increment(m_state.overflow); return {}; }
            for (auto& entry : m_entries)
            {
                if (entry.token.id) continue;
                entry = {{m_nextId, parent}, now, false, false};
                m_nextId = m_nextId == std::numeric_limits<uint64_t>::max() ? 0 : m_nextId + 1;
                ++m_outstanding;
                return entry.token;
            }
            Increment(m_state.invalid); return {};
        }
        StartInfo Start(Token token, uint64_t now) noexcept
        {
            if (!token.id) return {};
            auto* entry = Find(token.id);
            if (!entry || entry->token.parent != token.parent || entry->started)
            { Increment(m_state.invalid); return {}; }
            if (now < entry->queuedAt)
            { Increment(m_state.invalid); *entry = {}; --m_outstanding; return {}; }
            entry->started = true; Increment(m_state.started);
            return {true, entry->queuedAt, now};
        }
        void Invalid(Token token) noexcept
        {
            Increment(m_state.invalid);
            if (auto* entry = Find(token.id)) entry->invalid = true;
        }
        void Complete(Token token, JobRecord record) noexcept
        {
            if (!token.id) return;
            auto* entry = Find(token.id);
            if (!entry || entry->token.parent != token.parent || !entry->started)
            { Increment(m_state.invalid); return; }
            if (record.id != token.id || record.parentId != token.parent || !std::isfinite(record.totalMs) || record.totalMs < 0)
            { Increment(m_state.invalid); *entry = {}; --m_outstanding; return; }
            if (!record.valid && !entry->invalid) Increment(m_state.invalid);
            if (entry->invalid) record.valid = false;
            *entry = {}; --m_outstanding;
            Increment(m_state.completed);
            if (record.failed) Increment(m_state.failed);
            if (record.totalMs < m_state.slowThresholdMs) return;
            if (m_state.recordCount == m_state.records.size()) Increment(m_state.evictedSlow);
            else ++m_state.recordCount;
            m_state.records[m_nextRecord] = record;
            m_nextRecord = (m_nextRecord + 1) % m_state.records.size();
        }
        Snapshot GetSnapshot() const noexcept
        {
            Snapshot result = m_state;
            const size_t first = m_state.recordCount == m_state.records.size() ? m_nextRecord : 0;
            for (size_t i = 0; i < m_state.recordCount; ++i)
                result.records[i] = m_state.records[(first + i) % m_state.records.size()];
            return result;
        }
    private:
        struct Entry { Token token{}; uint64_t queuedAt{}; bool started{}, invalid{}; };
        Entry* Find(uint64_t id) noexcept
        {
            for (auto& entry : m_entries) if (entry.token.id == id) return &entry;
            return nullptr;
        }
        static void Increment(uint64_t& value) noexcept
        { if (value != std::numeric_limits<uint64_t>::max()) ++value; }
        Snapshot m_state{};
        std::array<Entry, Capacity> m_entries{};
        uint64_t m_nextId{1};
        size_t m_outstanding{}, m_nextRecord{};
    };

    inline JobRecord MakeRecord(Token token, uint64_t queuedAt, const std::array<uint64_t, 5>& times,
        uint32_t phaseMask, uint32_t kindMask, bool valid = true, bool failed = false) noexcept
    {
        const auto span = [&](size_t a, size_t b) {
            return (phaseMask & (uint32_t{1} << a)) && (phaseMask & (uint32_t{1} << b)) && times[b] >= times[a]
                ? double(times[b] - times[a]) / 1e6 : 0.0;
        };
        JobRecord record{};
        record.id = token.id; record.parentId = token.parent; record.phaseMask = phaseMask; record.kindMask = kindMask;
        record.queuedAt = double(queuedAt) / 1e9; record.startedAt = double(times[0]) / 1e9; record.endedAt = double(times[4]) / 1e9;
        record.queueWaitMs = times[0] >= queuedAt ? double(times[0] - queuedAt) / 1e6 : 0.0;
        record.callbackMs = span(0, 1); record.drainMs = span(1, 2); record.handleScopeMs = span(2, 3);
        record.executeTailMs = span(3, 4); record.totalMs = span(0, 4); record.valid = valid; record.failed = failed;
        return record;
    }
}
