#include "RuntimeJobDiagnosticsInternal.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Babylon::RuntimeDiagnostics;
using Babylon::RuntimeDiagnostics::Detail::Tracker;
using Babylon::RuntimeDiagnostics::Detail::MakeRecord;
namespace { std::atomic<bool> countAllocations{}; std::atomic<size_t> allocations{}; }
void* operator new(size_t size)
{
    if (countAllocations.load()) ++allocations;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }

int main()
{
    int checks = 0, failures = 0;
    const auto require = [&](bool value, const char* text) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << text << '\n'; } };
    static_assert(!std::is_copy_constructible_v<JobScope> && !std::is_move_constructible_v<JobScope>);
    static_assert(std::is_trivially_copyable_v<JobRecord> && std::is_trivially_copyable_v<Snapshot>);
    Tracker tracker;
    require(!tracker.Enabled() && tracker.Enqueue(0, 0).id == 0, "new observation defaults off");
    tracker.Configure(true);
    const auto token = tracker.Enqueue(0, 1000000);
    require(token.id == 1 && token.parent == 0, "first enqueued identity is monotonic");
    const auto start = tracker.Start(token, 3000000);
    require(start.accepted && start.queuedAt == 1000000, "start retains original enqueue time");
    require(!tracker.Start(token, 3000001).accepted && tracker.GetSnapshot().invalid == 1, "duplicate start does not steal active owner");
    auto record = MakeRecord(token, start.queuedAt, {3000000, 7000000, 8000000, 9000000, 11000000}, 31, KindRaf);
    require(record.queueWaitMs == 2 && record.callbackMs == 4 && record.drainMs == 1 && record.handleScopeMs == 1 &&
        record.executeTailMs == 2 && record.totalMs == 8, "deterministic adjacent wall stages and total remain distinct");
    tracker.Complete(token, record);
    auto snapshot = tracker.GetSnapshot();
    require(snapshot.completed == 1 && snapshot.recordCount == 1 && snapshot.records[0].id == token.id &&
        snapshot.records[0].valid, "only completed slow records are retained");
    tracker.Complete(token, record);
    require(tracker.GetSnapshot().completed == 1 && tracker.GetSnapshot().invalid == 2, "duplicate completion cannot duplicate history");
    const auto partial = tracker.Enqueue(token.id, 20000000);
    require(partial.parent == token.id && partial.id > token.id, "parent identity is data without ownership");
    tracker.Start(partial, 21000000);
    tracker.Complete(partial, MakeRecord(partial, 20000000, {21000000, 0, 0, 0, 24000000}, 17, 0, true, true));
    snapshot = tracker.GetSnapshot();
    require(snapshot.failed == 1 && snapshot.records[1].valid && snapshot.records[1].phaseMask == 17 &&
        snapshot.records[1].callbackMs == 0 && snapshot.records[1].executeTailMs == 0,
        "partial unwind keeps mask and invents no missing duration");
    const auto fast = tracker.Enqueue(0, 30000000); tracker.Start(fast, 30000000);
    tracker.Complete(fast, MakeRecord(fast, 30000000, {30000000, 30000001, 30000002, 30000003, 30000004}, 31, 0));
    require(tracker.GetSnapshot().completed == 3 && tracker.GetSnapshot().recordCount == 2, "fast jobs count without filling slow retention");
    const auto beforeDisable = tracker.GetSnapshot(); tracker.Configure(false);
    require(tracker.Enqueue(0, 0).id == 0 && tracker.GetSnapshot().completed == beforeDisable.completed &&
        tracker.GetSnapshot().recordCount == beforeDisable.recordCount, "disable neither enqueues nor resets lifetime history");
    tracker.Configure(true);
    const auto old = tracker.Enqueue(0, 50000000); tracker.Configure(false);
    require(tracker.Start(old, 51000000).accepted, "already queued observation may begin after disable");
    tracker.Complete(old, MakeRecord(old, 50000000, {51000000, 52000000, 53000000, 54000000, 55000000}, 31, 0));
    require(tracker.GetSnapshot().completed == beforeDisable.completed + 1, "inflight finish remains observable after disable");
    Tracker unsupported{false}; unsupported.Configure(true);
    require(!unsupported.Enabled() && !unsupported.GetSnapshot().available && unsupported.Enqueue(0, 0).id == 0,
        "unsupported compilation cannot invent available instrumentation");
    Tracker bounded; bounded.Configure(true);
    std::array<Token, Tracker::Capacity> pending{};
    for (auto& value : pending) value = bounded.Enqueue(0, 100);
    require(bounded.Enqueue(0, 100).id == 0 && bounded.GetSnapshot().overflow == 1, "outstanding token storage is bounded without backpressure");
    for (const auto value : pending) { bounded.Start(value, 100); bounded.Complete(value, MakeRecord(value, 100, {100, 101, 102, 103, 104}, 31, 0)); }
    require(bounded.Enqueue(0, 200).id > pending.back().id, "released slots do not reuse identity");
    Tracker exhausted{true, std::numeric_limits<uint64_t>::max()}; exhausted.Configure(true);
    require(exhausted.Enqueue(0, 0).id == std::numeric_limits<uint64_t>::max() && exhausted.Enqueue(0, 0).id == 0 &&
        exhausted.GetSnapshot().overflow == 1, "identity exhaustion refuses wraparound");
    Tracker history; history.Configure(true);
    uint64_t first = 0;
    for (size_t i = 0; i < 70; ++i)
    {
        const auto value = history.Enqueue(0, 10000000 * i); history.Start(value, 10000000 * i);
        if (!first) first = value.id;
        history.Complete(value, MakeRecord(value, 10000000 * i,
            {10000000 * i, 10000000 * i + 1000000, 10000000 * i + 1000000, 10000000 * i + 1000000, 10000000 * i + 1000000}, 31, 0));
    }
    snapshot = history.GetSnapshot();
    require(snapshot.recordCount == 64 && snapshot.evictedSlow == 6 && snapshot.records.front().id == first + 6 &&
        snapshot.records.back().id == first + 69, "slow ring retains64 recent completed records in completion order");
    Tracker reverse; reverse.Configure(true); const auto backwards = reverse.Enqueue(0, 10);
    require(!reverse.Start(backwards, 9).accepted && reverse.GetSnapshot().invalid == 1, "backward start cannot underflow queue delay");

    Configure(true);
    if (!GetSnapshot().available)
    {
        const auto before = GetSnapshot();
        require(!before.enabled && Enqueue().id == 0, "unsupported public profiler stays unavailable and disabled");
        { auto job = Begin(Enqueue()); TagCurrentJob(KindRaf); job.MarkCallbackDone(); job.MarkDrainDone(); job.MarkHandleScopeDone(); job.Finish(); }
        require(GetSnapshot().enqueued == before.enqueued && GetSnapshot().completed == before.completed,
            "unsupported wrapper path creates no observations");
        Detail::RecordObserverFailure();
        require(GetSnapshot().invalid == before.invalid + 1 && GetSnapshot().invalid == before.invalid + 1,
            "observer failure remains visible without resetting even when unsupported");
        std::cout << "RuntimeJobDiagnostics unsupported: " << checks << " checks, " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    require(GetSnapshot().available && GetSnapshot().enabled, "supported public profiler enables explicitly");
    const auto baseline = GetSnapshot();
    const auto outerToken = Enqueue();
    {
        auto outer = Begin(outerToken); TagCurrentJob(KindRaf);
        const auto childToken = Enqueue(); require(childToken.parent == outerToken.id, "TLS names current parent job");
        { auto child = Begin(childToken); TagCurrentJob(KindCommandDeferred); child.MarkCallbackDone(); child.MarkDrainDone(); child.MarkHandleScopeDone(); }
        const auto secondChild = Enqueue(); require(secondChild.parent == outerToken.id, "nested finish restores the original TLS parent");
        { auto child = Begin(secondChild); child.Finish(); child.Finish(); }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        outer.MarkCallbackDone(); outer.MarkDrainDone(); outer.MarkHandleScopeDone(); outer.Finish(); outer.Finish();
    }
    snapshot = GetSnapshot();
    require(snapshot.completed == baseline.completed + 3 && snapshot.recordCount > 0 && snapshot.records[snapshot.recordCount - 1].id == outerToken.id &&
        snapshot.records[snapshot.recordCount - 1].kindMask == KindRaf, "public finish is idempotent and tags only completed job data");
    bool caught = false;
    try { auto job = Begin(Enqueue()); std::this_thread::sleep_for(std::chrono::milliseconds{2}); throw std::runtime_error{"original"}; }
    catch (const std::runtime_error& e) { caught = std::string{e.what()} == "original"; }
    require(caught && GetSnapshot().failed == baseline.failed + 1, "destructor observes unwind without intercepting original exception");
    const auto beforeThreads = GetSnapshot();
    std::atomic<bool> stopReader{};
    std::thread reader{[&] { while (!stopReader.load()) { auto s = GetSnapshot(); if (s.recordCount > 64) std::abort(); } }};
    std::vector<std::thread> workers;
    for (size_t i = 0; i < 6; ++i) workers.emplace_back([] {
        for (size_t j = 0; j < 100; ++j) { auto job = Begin(Enqueue()); job.MarkCallbackDone(); job.MarkDrainDone(); job.MarkHandleScopeDone(); }
    });
    for (auto& worker : workers) worker.join(); stopReader = true; reader.join();
    snapshot = GetSnapshot();
    require(snapshot.completed == beforeThreads.completed + 600 && snapshot.started == beforeThreads.started + 600 &&
        snapshot.invalid == beforeThreads.invalid && snapshot.overflow == beforeThreads.overflow,
        "six workers and concurrent snapshots preserve exact counts and bounded storage");
    const auto beforeAlloc = allocations.load(); countAllocations = true;
    { auto job = Begin(Enqueue()); TagCurrentJob(KindTextureReady); job.MarkCallbackDone(); job.MarkDrainDone(); job.MarkHandleScopeDone(); job.Finish(); (void)GetSnapshot(); }
    countAllocations = false;
    require(allocations == beforeAlloc, "warm observation path allocates no heap storage");
    const auto beforeInvalid = GetSnapshot();
    { auto job = Begin(Enqueue()); job.MarkDrainDone(); job.MarkCallbackDone(); job.MarkDrainDone(); job.MarkHandleScopeDone(); }
    require(GetSnapshot().invalid == beforeInvalid.invalid + 1, "out-of-order mark invalidates observation without blocking completion");
    {
        const auto outerId = Enqueue(); auto outer = Begin(outerId); auto inner = Begin(Enqueue());
        std::this_thread::sleep_for(std::chrono::milliseconds{2}); outer.Finish(); inner.Finish();
        const auto after = GetSnapshot(); bool retainedInvalid = false;
        for (size_t i = 0; i < after.recordCount; ++i) if (after.records[i].id == outerId.id) retainedInvalid = !after.records[i].valid;
        require(retainedInvalid, "retained out-of-order finish record is explicitly invalid");
        const auto next = Enqueue(); require(next.parent == 0, "out-of-order finish cannot leave a dangling TLS parent");
        auto final = Begin(next); final.Finish();
    }
    Configure(false); const auto disabled = GetSnapshot();
    { auto job = Begin(Enqueue()); job.MarkCallbackDone(); job.MarkDrainDone(); job.MarkHandleScopeDone(); }
    require(!GetSnapshot().enabled && GetSnapshot().enqueued == disabled.enqueued && GetSnapshot().completed == disabled.completed,
        "disabled public path creates no observation or counter change");
    const auto beforeMissing = GetSnapshot(); Detail::RecordObserverFailure();
    require(GetSnapshot().invalid == beforeMissing.invalid + 1 && GetSnapshot().invalid == beforeMissing.invalid + 1,
        "atomic observer failures merge into lifetime invalid count without reset");
    std::cout << "RuntimeJobDiagnostics: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
