#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Babylon::RuntimeDiagnostics
{
    inline constexpr uint32_t KindRaf = 1, KindRafDeferred = 2, KindCommandDeferred = 4, KindTextureReady = 8;
    struct Token { uint64_t id{}, parent{}; };
    struct JobRecord
    {
        uint64_t id{}, parentId{};
        uint32_t kindMask{}, phaseMask{};
        double queuedAt{}, startedAt{}, endedAt{}, queueWaitMs{}, callbackMs{}, drainMs{}, handleScopeMs{}, executeTailMs{}, totalMs{};
        bool valid{true}, failed{};
    };
    struct Snapshot
    {
        bool available{}, enabled{};
        uint64_t enqueued{}, started{}, completed{}, failed{}, invalid{}, overflow{}, evictedSlow{};
        double slowThresholdMs{1.0};
        size_t recordCount{};
        // Completed jobs >=threshold only, oldest retained completion first.
        // Missing fast/evicted jobs do not imply a fully observed queue timeline.
        std::array<JobRecord, 64> records{};
    };

    // Configure before runtime creation. Disable stops new observations but leaves
    // outstanding scopes and all process-lifetime counters/retention intact.
    void Configure(bool enabled) noexcept;
    Snapshot GetSnapshot() noexcept;
    Token Enqueue() noexcept;

    class JobScope final
    {
    public:
        ~JobScope() noexcept;
        JobScope(const JobScope&) = delete;
        JobScope& operator=(const JobScope&) = delete;
        JobScope(JobScope&&) = delete;
        JobScope& operator=(JobScope&&) = delete;
        void MarkCallbackDone() noexcept;
        void MarkDrainDone() noexcept;
        void MarkHandleScopeDone() noexcept;
        void Finish() noexcept;
    private:
        friend JobScope Begin(Token) noexcept;
        friend Token Enqueue() noexcept;
        friend void TagCurrentJob(uint32_t) noexcept;
        explicit JobScope(Token) noexcept;
        void Mark(size_t phase) noexcept;
        void Invalid() noexcept;
        Token m_token{};
        JobScope* m_previous{};
        std::array<uint64_t, 5> m_times{};
        uint64_t m_queuedAt{};
        uint32_t m_phaseMask{}, m_kindMask{};
        size_t m_lastPhase{};
        int m_exceptions{};
        bool m_active{}, m_valid{true};
    };
    // Guaranteed C++17 prvalue construction: no move, retained callback or task owner.
    JobScope Begin(Token token) noexcept;
    void TagCurrentJob(uint32_t kindBits) noexcept;
}
