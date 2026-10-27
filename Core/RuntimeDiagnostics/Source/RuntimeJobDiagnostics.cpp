#include "RuntimeJobDiagnosticsInternal.h"
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <limits>

#ifndef BABYLON_RUNTIME_DIAGNOSTICS_SUPPORTED
#define BABYLON_RUNTIME_DIAGNOSTICS_SUPPORTED 0
#endif

namespace Babylon::RuntimeDiagnostics
{
    namespace
    {
        constexpr bool supported = BABYLON_RUNTIME_DIAGNOSTICS_SUPPORTED != 0;
        std::atomic<bool> enabled{};
        std::atomic<uint64_t> observerFailures{};
        thread_local JobScope* current{};
        struct State { std::mutex mutex; Detail::Tracker tracker{supported}; };
        State& Global() { static State state; return state; }
        uint64_t Now() noexcept
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        Snapshot WithObserverFailures(Snapshot result) noexcept
        {
            const uint64_t missing = observerFailures.load(std::memory_order_relaxed);
            const uint64_t max = std::numeric_limits<uint64_t>::max();
            result.invalid = max - result.invalid < missing ? max : result.invalid + missing;
            return result;
        }
    }
    void Detail::RecordObserverFailure() noexcept
    {
        uint64_t old = observerFailures.load(std::memory_order_relaxed);
        while (old != std::numeric_limits<uint64_t>::max() && !observerFailures.compare_exchange_weak(
            old, old + 1, std::memory_order_relaxed)) {}
    }
    void Configure(bool value) noexcept
    {
        try { auto& state = Global(); const std::lock_guard lock{state.mutex};
            state.tracker.Configure(value); enabled.store(state.tracker.Enabled(), std::memory_order_release); }
        catch (...) { Detail::RecordObserverFailure(); enabled.store(false, std::memory_order_release); }
    }
    Snapshot GetSnapshot() noexcept
    {
        try { auto& state = Global(); const std::lock_guard lock{state.mutex}; return WithObserverFailures(state.tracker.GetSnapshot()); }
        catch (...) { Detail::RecordObserverFailure(); Snapshot result{}; result.available = supported; return WithObserverFailures(result); }
    }
    Token Enqueue() noexcept
    {
        if (!enabled.load(std::memory_order_acquire)) return {};
        try { const uint64_t now = Now(); auto& state = Global(); const std::lock_guard lock{state.mutex};
            return state.tracker.Enqueue(current ? current->m_token.id : 0, now); }
        catch (...) { Detail::RecordObserverFailure(); return {}; }
    }
    JobScope::JobScope(Token token) noexcept : m_token{token}
    {
        if (!token.id) return;
        try { const uint64_t now = Now(); auto& state = Global(); const std::lock_guard lock{state.mutex};
            const auto start = state.tracker.Start(token, now); if (!start.accepted) return;
            m_queuedAt = start.queuedAt; m_times[0] = start.startedAt; m_phaseMask = 1;
            m_exceptions = std::uncaught_exceptions(); m_previous = current; current = this; m_active = true; }
        catch (...) { Detail::RecordObserverFailure(); }
    }
    JobScope Begin(Token token) noexcept { return JobScope{token}; }
    JobScope::~JobScope() noexcept { Finish(); }
    void JobScope::Invalid() noexcept
    {
        m_valid = false;
        try { auto& state = Global(); const std::lock_guard lock{state.mutex}; state.tracker.Invalid(m_token); }
        catch (...) { Detail::RecordObserverFailure(); }
    }
    void JobScope::Mark(size_t phase) noexcept
    {
        if (!m_active) return;
        const uint64_t now = Now();
        if (phase != m_lastPhase + 1 || phase > 3 || now < m_times[m_lastPhase]) { Invalid(); return; }
        m_times[phase] = now; m_phaseMask |= uint32_t{1} << phase; m_lastPhase = phase;
    }
    void JobScope::MarkCallbackDone() noexcept { Mark(1); }
    void JobScope::MarkDrainDone() noexcept { Mark(2); }
    void JobScope::MarkHandleScopeDone() noexcept { Mark(3); }
    void JobScope::Finish() noexcept
    {
        if (!m_active) return;
        uint64_t now = Now();
        if (now < m_times[m_lastPhase]) { Invalid(); now = m_times[m_lastPhase]; }
        m_times[4] = now; m_phaseMask |= 16;
        // Restore TLS regardless of observation failure; no task or callback is retained.
        // Also splice an invalid out-of-order explicit finish out of a nested chain,
        // so a later inner finish cannot restore a pointer to a destroyed observer.
        if (current == this) current = m_previous;
        else
        {
            Invalid();
            for (auto* item = current; item; item = item->m_previous)
                if (item->m_previous == this) { item->m_previous = m_previous; break; }
        }
        m_active = false;
        const auto record = Detail::MakeRecord(m_token, m_queuedAt, m_times, m_phaseMask, m_kindMask,
            m_valid, std::uncaught_exceptions() > m_exceptions);
        try { auto& state = Global(); const std::lock_guard lock{state.mutex}; state.tracker.Complete(m_token, record); }
        catch (...) { Detail::RecordObserverFailure(); }
    }
    void TagCurrentJob(uint32_t bits) noexcept
    {
        if (!current || !current->m_active) return;
        if (bits & ~uint32_t{15}) { current->Invalid(); return; }
        current->m_kindMask |= bits;
    }
}
