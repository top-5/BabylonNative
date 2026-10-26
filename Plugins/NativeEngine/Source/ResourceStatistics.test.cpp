// CPU-only helper test: no graphics device, JavaScript runtime or bgfx objects.
#include "ResourceStatisticsInternal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace Babylon::Plugins::NativeEngine::Detail;
    struct FakeClock
    {
        using duration = std::chrono::nanoseconds;
        using time_point = std::chrono::time_point<FakeClock>;
        static inline duration current{};
        static time_point now() noexcept { return time_point{current}; }
    };
}

int main()
{
    int checks = 0, failed = 0;
    const auto Require = [&](bool value, const char* description)
    {
        ++checks;
        if (!value) { ++failed; std::cerr << "FAIL: " << description << '\n'; }
    };
    ResourceCounter counter;
    Require(counter.Snapshot().calls == 0, "fresh counter is empty");
    {
        ResourceScope<FakeClock> background{counter, 999, false};
        FakeClock::current += std::chrono::nanoseconds{500};
        background.Succeed();
    }
    Require(counter.Snapshot().calls == 0 && counter.Snapshot().bytes == 0,
        "disabled background preparation does not contaminate load-image counters");
    {
        ResourceScope<FakeClock> scope{counter, 100};
        FakeClock::current += std::chrono::nanoseconds{13};
        scope.Succeed();
        Require(counter.Snapshot().calls == 0, "in-flight work is not reported as completed");
    }
    auto stats = counter.Snapshot();
    Require(stats.calls == 1 && stats.succeeded == 1 && stats.failed == 0 && stats.totalNs == 13 && stats.maxNs == 13 && stats.bytes == 100,
        "successful measured operation records deterministic duration and bytes");
    bool caught = false;
    try
    {
        ResourceScope<FakeClock> scope{counter, 200};
        FakeClock::current += std::chrono::nanoseconds{29};
        throw std::runtime_error{"original error"};
    }
    catch (const std::runtime_error& error) { caught = std::string{error.what()} == "original error"; }
    stats = counter.Snapshot();
    Require(caught, "measurement preserves the original exception");
    Require(stats.calls == 2 && stats.succeeded == 1 && stats.failed == 1 && stats.totalNs == 42 && stats.maxNs == 29 && stats.bytes == 300,
        "unwinding records failure and its elapsed work");
    {
        ResourceScope<FakeClock> scope{counter, 10};
        FakeClock::current -= std::chrono::nanoseconds{1};
    }
    stats = counter.Snapshot();
    Require(stats.calls == 3 && stats.failed == 2 && stats.totalNs == 42 && stats.maxNs == 29 && stats.bytes == 310,
        "unfinished scope fails and a backwards test clock cannot underflow duration");

    ResourceCounter parentPreparation, childMip;
    ResourceCounter explicitlyFinished;
    {
        ResourceScope<FakeClock> scope{explicitlyFinished, 0};
        FakeClock::current += std::chrono::nanoseconds{7}; scope.Succeed(); scope.Finish();
        FakeClock::current += std::chrono::nanoseconds{50}; scope.Finish();
    }
    Require(explicitlyFinished.Snapshot().calls == 1 && explicitlyFinished.Snapshot().totalNs == 7,
        "explicit timing finish neither extends owner lifetime nor double-counts on destruction");
    FakeClock::current = std::chrono::nanoseconds{0};
    {
        ResourceScope<FakeClock> parent{parentPreparation, 1000};
        FakeClock::current += std::chrono::nanoseconds{10};
        {
            ResourceScope<FakeClock> child{childMip, 1000};
            FakeClock::current += std::chrono::nanoseconds{80};
            child.Succeed();
        }
        FakeClock::current += std::chrono::nanoseconds{10};
        parent.Succeed();
    }
    Require(parentPreparation.Snapshot().totalNs == 100 && childMip.Snapshot().totalNs == 80,
        "nested mip span overlaps its preparation parent and cannot be added to it");
    Require(parentPreparation.Snapshot().calls == 1 && childMip.Snapshot().calls == 1,
        "nested operation counters remain independent completed attempts");

    ResourceCounter concurrent;
    constexpr uint64_t workers = 6, iterations = 5000;
    std::atomic<uint64_t> done{};
    std::array<std::thread, workers> threads;
    for (uint64_t worker = 0; worker < workers; ++worker)
        threads[worker] = std::thread{[&, worker]
        {
            for (uint64_t i = 0; i < iterations; ++i) concurrent.Record(worker + 1, 8, i % 2 == 0);
            done.fetch_add(1, std::memory_order_relaxed);
        }};
    auto previous = concurrent.Snapshot();
    bool monotonic = true;
    while (done.load(std::memory_order_relaxed) != workers)
    {
        const auto now = concurrent.Snapshot();
        monotonic = monotonic && now.calls >= previous.calls && now.succeeded >= previous.succeeded && now.failed >= previous.failed &&
            now.totalNs >= previous.totalNs && now.maxNs >= previous.maxNs && now.bytes >= previous.bytes;
        previous = now;
        std::this_thread::yield();
    }
    for (auto& thread : threads) thread.join();
    stats = concurrent.Snapshot();
    Require(monotonic, "worker writes and live per-field snapshots stay monotonic");
    Require(stats.calls == workers * iterations && stats.succeeded == workers * iterations / 2 && stats.failed == workers * iterations / 2,
        "concurrent completion counts have no lost updates");
    Require(stats.totalNs == 21 * iterations && stats.maxNs == 6 && stats.bytes == workers * iterations * 8,
        "concurrent duration totals, maximum and byte totals are exact after join");
    concurrent.Record(1, 0, true);
    const auto after = concurrent.Snapshot();
    Require(after.calls == stats.calls + 1 && after.totalNs == stats.totalNs + 1 && after.maxNs == 6,
        "new windows add to process-lifetime totals without resetting maximum");
    Counter(ResourcePhase::ParseImage).Record(11, 101, true);
    Counter(ResourcePhase::PrepareImage).Record(22, 202, false);
    Counter(ResourcePhase::TextureSubmit).Record(33, 303, true);
    Counter(ResourcePhase::ImageFlip).Record(44, 404, true);
    Counter(ResourcePhase::FormatConvert).Record(55, 505, false);
    Counter(ResourcePhase::MipGenerate).Record(66, 606, true);
    const auto publicStats = Babylon::Plugins::NativeEngine::GetResourceStatistics();
    Require(publicStats.parseImage.calls == 1 && publicStats.parseImage.totalNs == 11 && publicStats.parseImage.bytes == 101,
        "public snapshot maps parse-image counters exactly");
    Require(publicStats.prepareImage.calls == 1 && publicStats.prepareImage.failed == 1 && publicStats.prepareImage.totalNs == 22,
        "public snapshot maps preparation failure counters exactly");
    Require(publicStats.textureSubmit.calls == 1 && publicStats.textureSubmit.succeeded == 1 && publicStats.textureSubmit.bytes == 303,
        "public snapshot maps CPU texture submission counters exactly");
    Require(publicStats.imageFlip.calls == 1 && publicStats.imageFlip.totalNs == 44 && publicStats.imageFlip.bytes == 404,
        "public snapshot maps image-flip work independently");
    Require(publicStats.formatConvert.calls == 1 && publicStats.formatConvert.failed == 1 && publicStats.formatConvert.totalNs == 55,
        "public snapshot maps format-conversion failure independently");
    Require(publicStats.mipGenerate.calls == 1 && publicStats.mipGenerate.succeeded == 1 && publicStats.mipGenerate.bytes == 606,
        "public snapshot maps mip-generation work independently");
    const auto replay = Babylon::Plugins::NativeEngine::GetResourceStatistics();
    Require(replay.parseImage.calls == 1 && replay.prepareImage.failed == 1 && replay.textureSubmit.bytes == 303,
        "reading public snapshots never resets counters");
    Require(replay.imageFlip.totalNs == 44 && replay.formatConvert.failed == 1 && replay.mipGenerate.totalNs == 66,
        "reading public snapshots preserves all nested-span lifetime counters");
    Require(!Babylon::Plugins::NativeEngine::GetResourceStatistics().commandTimingEnabled,
        "native command timing defaults off");
    Babylon::Plugins::NativeEngine::SetCommandTimingEnabled(true);
    for (auto phase : {ResourcePhase::CommandRead, ResourcePhase::CommandScopeDispatch, ResourcePhase::CommandExecute,
        ResourcePhase::VertexBufferCreate, ResourcePhase::IndexBufferCreate, ResourcePhase::VertexBufferRecord, ResourcePhase::RafCallbacks})
        Counter(phase).Record(77, 0, true);
    const auto commands = Babylon::Plugins::NativeEngine::GetResourceStatistics();
    Require(commands.commandTimingEnabled && commands.commandRead.totalNs == 77 && commands.commandScopeDispatch.totalNs == 77 &&
        commands.commandExecute.totalNs == 77 && commands.vertexBufferCreate.totalNs == 77 && commands.indexBufferCreate.totalNs == 77 &&
        commands.vertexBufferRecord.totalNs == 77 && commands.rafCallbacks.totalNs == 77, "all optional command spans map separately");
    Babylon::Plugins::NativeEngine::SetCommandTimingEnabled(false);
    Require(!Babylon::Plugins::NativeEngine::GetResourceStatistics().commandTimingEnabled &&
        Babylon::Plugins::NativeEngine::GetResourceStatistics().commandRead.totalNs == 77, "disabling command clocks does not reset lifetime counters");
    std::cout << "ResourceStatistics: " << checks << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
