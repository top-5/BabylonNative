#pragma once

#include <Babylon/Plugins/NativeEngineResourceStatistics.h>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace Babylon::Plugins::NativeEngine::Detail
{
    class ResourceCounter final
    {
    public:
        void Record(uint64_t elapsedNs, uint64_t bytes, bool succeeded) noexcept
        {
            m_calls.fetch_add(1, std::memory_order_relaxed);
            (succeeded ? m_succeeded : m_failed).fetch_add(1, std::memory_order_relaxed);
            m_totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
            m_bytes.fetch_add(bytes, std::memory_order_relaxed);
            uint64_t previous = m_maxNs.load(std::memory_order_relaxed);
            while (previous < elapsedNs && !m_maxNs.compare_exchange_weak(previous, elapsedNs, std::memory_order_relaxed)) {}
        }

        ResourceOperationStatistics Snapshot() const noexcept
        {
            return {m_calls.load(std::memory_order_relaxed), m_succeeded.load(std::memory_order_relaxed),
                m_failed.load(std::memory_order_relaxed), m_totalNs.load(std::memory_order_relaxed),
                m_maxNs.load(std::memory_order_relaxed), m_bytes.load(std::memory_order_relaxed)};
        }

    private:
        std::atomic<uint64_t> m_calls{}, m_succeeded{}, m_failed{}, m_totalNs{}, m_maxNs{}, m_bytes{};
    };

    enum class ResourcePhase { ParseImage, PrepareImage, TextureSubmit };
    ResourceCounter& Counter(ResourcePhase phase) noexcept;

    // Explicit Succeed is called only after the full operation (including its checks)
    // returns. Unwinding/early returns record failure without intercepting exceptions.
    template<class Clock = std::chrono::steady_clock>
    class ResourceScope final
    {
    public:
        ResourceScope(ResourceCounter& counter, uint64_t bytes) noexcept
            : m_counter{counter}, m_bytes{bytes}, m_started{Clock::now()} {}
        ~ResourceScope() noexcept
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_started).count();
            m_counter.Record(elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0, m_bytes, m_succeeded);
        }
        ResourceScope(const ResourceScope&) = delete;
        ResourceScope& operator=(const ResourceScope&) = delete;
        void Succeed() noexcept { m_succeeded = true; }

    private:
        ResourceCounter& m_counter;
        uint64_t m_bytes;
        typename Clock::time_point m_started;
        bool m_succeeded{};
    };
}
