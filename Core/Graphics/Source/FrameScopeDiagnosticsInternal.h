#pragma once
#include <Babylon/Graphics/FrameScopeDiagnostics.h>
#include <array>
#include <cstddef>
#include <limits>

namespace Babylon::Graphics::Detail
{
    // Caller holds the existing frame synchronization mutex for every operation.
    // Fixed storage only: observes tickets/timestamps, never scope ownership.
    class FrameScopeTracker final
    {
    public:
        static constexpr size_t Capacity = 256;

        // The seed exists for deterministic exhaustion tests; zero means exhausted.
        explicit FrameScopeTracker(uint64_t nextIdentity = 1) noexcept : m_nextIdentity{nextIdentity} {}

        void BeginFrame() noexcept
        {
            if (m_activeCount != 0)
            {
                m_snapshot.coverageComplete = false;
                return;
            }
            m_snapshot = {};
        }

        uint64_t Acquire(FrameScopeKind kind, uint64_t nowNs) noexcept
        {
            Increment(m_snapshot.acquired);
            if (!ValidKind(kind)) { Invalid(); return 0; }
            if (m_activeCount == Capacity || m_nextIdentity == 0)
            {
                Increment(m_snapshot.overflow);
                m_snapshot.coverageComplete = false;
                return 0;
            }
            for (auto& entry : m_entries)
            {
                if (entry.identity != 0) continue;
                entry = {};
                entry.identity = m_nextIdentity;
                m_nextIdentity = m_nextIdentity == std::numeric_limits<uint64_t>::max() ? 0 : m_nextIdentity + 1;
                entry.kind = kind;
                entry.times[0] = nowNs;
                entry.lastTime = nowNs;
                entry.phaseMask = Bit(FrameScopePhase::Acquired);
                ++m_activeCount;
                return entry.identity;
            }
            // Capacity and entry state must agree; fail observation closed if not.
            Invalid();
            return 0;
        }

        void Mark(uint64_t identity, FrameScopePhase phase, uint64_t nowNs) noexcept
        {
            if (identity == 0) return;
            Entry* entry = Find(identity);
            if (!entry || !ValidPhase(phase) || nowNs < entry->lastTime || !Expected(*entry, phase))
            {
                Invalid();
                return;
            }
            entry->times[static_cast<size_t>(phase)] = nowNs;
            entry->phaseMask |= Bit(phase);
            entry->lastPhase = phase;
            entry->lastTime = nowNs;
        }

        void Release(uint64_t identity, uint64_t nowNs) noexcept
        {
            if (identity == 0) return;
            Entry* entry = Find(identity);
            if (!entry) { Invalid(); return; }
            Increment(m_snapshot.released);
            if (nowNs < entry->lastTime)
            {
                Invalid();
            }
            else
            {
                FrameScopeRecord record{};
                record.identity = entry->identity;
                record.acquiredAtNs = entry->times[0];
                record.kind = entry->kind;
                record.ageNs = nowNs - entry->times[0];
                record.phaseMask = entry->phaseMask;
                record.acquiredToJsNs = Between(*entry, FrameScopePhase::Acquired, FrameScopePhase::JsStarted);
                record.callbackNs = Between(*entry, FrameScopePhase::JsStarted, FrameScopePhase::CallbackReturned);
                record.deferredWaitNs = Between(*entry, FrameScopePhase::DeferredQueued, FrameScopePhase::DeferredStarted);
                if (Has(*entry, FrameScopePhase::DeferredStarted))
                    record.afterDeferredNs = nowNs - entry->times[static_cast<size_t>(FrameScopePhase::DeferredStarted)];
                if (m_snapshot.longest.identity == 0 || record.ageNs > m_snapshot.longest.ageNs ||
                    (record.ageNs == m_snapshot.longest.ageNs && record.identity < m_snapshot.longest.identity))
                    m_snapshot.longest = record;
            }
            *entry = {};
            --m_activeCount;
        }

        FrameScopeSnapshot Snapshot() const noexcept { return m_snapshot; }

    private:
        struct Entry
        {
            uint64_t identity{}, phaseMask{}, lastTime{};
            FrameScopeKind kind{FrameScopeKind::Other};
            FrameScopePhase lastPhase{FrameScopePhase::Acquired};
            std::array<uint64_t, 5> times{};
        };

        static constexpr bool ValidKind(FrameScopeKind kind) noexcept
        {
            return kind == FrameScopeKind::Other || kind == FrameScopeKind::Command || kind == FrameScopeKind::Raf;
        }
        static constexpr bool ValidPhase(FrameScopePhase phase) noexcept
        {
            return static_cast<uint8_t>(phase) <= static_cast<uint8_t>(FrameScopePhase::DeferredStarted);
        }
        static constexpr uint64_t Bit(FrameScopePhase phase) noexcept { return uint64_t{1} << static_cast<uint8_t>(phase); }
        static bool Has(const Entry& entry, FrameScopePhase phase) noexcept { return (entry.phaseMask & Bit(phase)) != 0; }
        static uint64_t Between(const Entry& entry, FrameScopePhase first, FrameScopePhase last) noexcept
        {
            return Has(entry, first) && Has(entry, last) ?
                entry.times[static_cast<size_t>(last)] - entry.times[static_cast<size_t>(first)] : 0;
        }
        static bool Expected(const Entry& entry, FrameScopePhase phase) noexcept
        {
            if (entry.kind == FrameScopeKind::Other) return false;
            if (entry.lastPhase == FrameScopePhase::Acquired)
                return phase == (entry.kind == FrameScopeKind::Raf ? FrameScopePhase::JsStarted : FrameScopePhase::DeferredQueued);
            if (entry.lastPhase == FrameScopePhase::JsStarted) return phase == FrameScopePhase::CallbackReturned;
            if (entry.lastPhase == FrameScopePhase::CallbackReturned) return phase == FrameScopePhase::DeferredQueued;
            if (entry.lastPhase == FrameScopePhase::DeferredQueued) return phase == FrameScopePhase::DeferredStarted;
            return false;
        }
        Entry* Find(uint64_t identity) noexcept
        {
            for (auto& entry : m_entries) if (entry.identity == identity) return &entry;
            return nullptr;
        }
        void Increment(uint64_t& value) noexcept
        {
            if (value == std::numeric_limits<uint64_t>::max()) m_snapshot.coverageComplete = false;
            else ++value;
        }
        void Invalid() noexcept
        {
            Increment(m_snapshot.invalidTransitions);
            m_snapshot.coverageComplete = false;
        }

        std::array<Entry, Capacity> m_entries{};
        size_t m_activeCount{};
        uint64_t m_nextIdentity{1};
        FrameScopeSnapshot m_snapshot{};
    };
}
