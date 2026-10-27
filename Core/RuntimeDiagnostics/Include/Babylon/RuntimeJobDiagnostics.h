#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Babylon::RuntimeDiagnostics
{
    inline constexpr uint32_t KindRaf = 1, KindRafDeferred = 2, KindCommandDeferred = 4, KindTextureReady = 8;
    enum class WorkKind : uint8_t { TextureNotify, ProgramCreate, ShaderProvider, ProgramInitialize, Count };
    inline constexpr size_t WorkKindCount = static_cast<size_t>(WorkKind::Count);
    struct Token { uint64_t id{}, parent{}; };
    struct JobRecord
    {
        uint64_t id{}, parentId{};
        uint32_t kindMask{}, phaseMask{};
        double queuedAt{}, startedAt{}, endedAt{}, queueWaitMs{}, callbackMs{}, drainMs{}, handleScopeMs{}, executeTailMs{}, totalMs{};
        bool valid{true}, failed{};
        // Inclusive wall spans; different categories can nest and are not additive.
        // TextureNotify surrounds a JS call and can include its automatic microtasks.
        std::array<uint64_t, WorkKindCount> workCalls{};
        std::array<double, WorkKindCount> workTotalMs{}, workMaxMs{};
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

    class WorkScope;
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
        friend class WorkScope;
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
        WorkScope* m_workTop{};
        std::array<uint8_t, WorkKindCount> m_workActive{};
        std::array<uint64_t, WorkKindCount> m_workCalls{};
        std::array<double, WorkKindCount> m_workTotalMs{}, m_workMaxMs{};
    };
    // Guaranteed C++17 prvalue construction: no move, retained callback or task owner.
    JobScope Begin(Token token) noexcept;
    void TagCurrentJob(uint32_t kindBits) noexcept;

    // Observation only, bound to the current thread's active job. No callbacks,
    // shared pointers or task owners are stored. Default-off/no-current does not
    // read the clock. Same-kind overlap, wrong finish order or outliving a job
    // makes that job's diagnostics invalid, without changing execution semantics.
    // Completed spans include exceptional returns; counts do not mean success.
    class WorkScope final
    {
    public:
        explicit WorkScope(WorkKind kind) noexcept;
        ~WorkScope() noexcept;
        WorkScope(const WorkScope&) = delete;
        WorkScope& operator=(const WorkScope&) = delete;
        WorkScope(WorkScope&&) = delete;
        WorkScope& operator=(WorkScope&&) = delete;
        void Finish() noexcept;
    private:
        friend class JobScope;
        JobScope* m_owner{};
        WorkScope* m_previous{};
        uint64_t m_started{};
        size_t m_kind{};
        bool m_active{};
    };
}
