#include "FrameScopeDiagnosticsInternal.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace Babylon::Graphics;
using Babylon::Graphics::Detail::FrameScopeTracker;

int main()
{
    int checks = 0, failures = 0;
    const auto require = [&](bool value, const char* message) {
        ++checks;
        if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    static_assert(std::is_trivially_copyable_v<FrameScopeRecord>);
    static_assert(std::is_trivially_copyable_v<FrameScopeSnapshot>);
    static_assert(noexcept(FrameScopeTracker{}.Acquire(FrameScopeKind::Other, 0)));

    FrameScopeTracker tracker;
    tracker.BeginFrame();
    auto snapshot = tracker.Snapshot();
    require(snapshot.coverageComplete && snapshot.acquired == 0 && snapshot.released == 0 && snapshot.longest.identity == 0,
        "empty frame is completely observed without invented scope");
    const auto command = tracker.Acquire(FrameScopeKind::Command, 100);
    tracker.Mark(command, FrameScopePhase::DeferredQueued, 110);
    tracker.Mark(command, FrameScopePhase::DeferredStarted, 300);
    snapshot = tracker.Snapshot();
    require(snapshot.acquired == 1 && snapshot.released == 0 && snapshot.longest.identity == 0,
        "deferred callback entry is not final scope release");
    tracker.Release(command, 350);
    snapshot = tracker.Snapshot();
    require(snapshot.coverageComplete && snapshot.released == 1 && snapshot.longest.identity == command &&
        snapshot.longest.kind == FrameScopeKind::Command && snapshot.longest.ageNs == 250,
        "command age ends at actual final release");
    require(snapshot.longest.phaseMask == 25 && snapshot.longest.deferredWaitNs == 190 &&
        snapshot.longest.afterDeferredNs == 50 && snapshot.longest.acquiredToJsNs == 0 && snapshot.longest.callbackNs == 0,
        "command records only observed phase endpoints");

    const auto raf = tracker.Acquire(FrameScopeKind::Raf, 1000);
    tracker.Mark(raf, FrameScopePhase::JsStarted, 1020);
    tracker.Mark(raf, FrameScopePhase::CallbackReturned, 1100);
    tracker.Mark(raf, FrameScopePhase::DeferredQueued, 1110);
    tracker.Mark(raf, FrameScopePhase::DeferredStarted, 1500);
    tracker.Release(raf, 1600);
    snapshot = tracker.Snapshot();
    require(snapshot.longest.identity == raf && snapshot.longest.ageNs == 600 && snapshot.longest.phaseMask == 31,
        "RAF longest scope preserves complete phase mask");
    require(snapshot.longest.acquiredToJsNs == 20 && snapshot.longest.callbackNs == 80 &&
        snapshot.longest.deferredWaitNs == 390 && snapshot.longest.afterDeferredNs == 100,
        "RAF phase intervals are exact and do not absorb unobserved intervals");
    require(snapshot.acquired == 2 && snapshot.released == 2 && snapshot.invalidTransitions == 0,
        "completed normal scopes preserve exact counters");

    tracker.BeginFrame();
    const auto partial = tracker.Acquire(FrameScopeKind::Raf, 50);
    tracker.Mark(partial, FrameScopePhase::JsStarted, 60);
    tracker.Release(partial, 90);
    snapshot = tracker.Snapshot();
    require(snapshot.coverageComplete && snapshot.longest.ageNs == 40 && snapshot.longest.phaseMask == 3 &&
        snapshot.longest.acquiredToJsNs == 10 && snapshot.longest.callbackNs == 0 && snapshot.longest.afterDeferredNs == 0,
        "exception or cancellation release retains partial mask without fabricated durations");
    const auto other = tracker.Acquire(FrameScopeKind::Other, 100);
    tracker.Release(other, 100);
    require(tracker.Snapshot().coverageComplete && tracker.Snapshot().released == 2,
        "other scopes need no dispatch phase and zero age is valid");
    const auto live = tracker.Acquire(FrameScopeKind::Command, 200);
    const auto beforeReset = tracker.Snapshot();
    tracker.BeginFrame();
    snapshot = tracker.Snapshot();
    require(!snapshot.coverageComplete && snapshot.acquired == beforeReset.acquired &&
        snapshot.released == beforeReset.released && snapshot.longest.identity == beforeReset.longest.identity,
        "BeginFrame with active entries refuses to discard observation");
    tracker.Release(live, 300);
    require(tracker.Snapshot().released == 3, "scope survives refused BeginFrame and can release normally");
    tracker.BeginFrame();
    snapshot = tracker.Snapshot();
    require(snapshot.coverageComplete && snapshot.acquired == 0 && snapshot.released == 0 && snapshot.longest.identity == 0,
        "quiescent BeginFrame resets only per-frame counters");
    const auto next = tracker.Acquire(FrameScopeKind::Other, 0);
    require(next > live, "identity sequence survives frame resets");
    auto movedTicket = next;
    uint64_t movedFrom = 0;
    tracker.Release(movedFrom, 1);
    tracker.Mark(movedFrom, FrameScopePhase::DeferredStarted, 1);
    require(tracker.Snapshot().released == 0 && tracker.Snapshot().invalidTransitions == 0,
        "moved-from zero ticket operations have no observational effects");
    tracker.Release(movedTicket, 1);
    require(tracker.Snapshot().released == 1 && tracker.Snapshot().longest.identity == next,
        "transferred ticket releases exactly its original scope identity");
    tracker.Release(next, 2);
    require(tracker.Snapshot().invalidTransitions == 1 && !tracker.Snapshot().coverageComplete,
        "duplicate final release is refused rather than counted twice");

    FrameScopeTracker invalid;
    const auto bad = invalid.Acquire(FrameScopeKind::Raf, 100);
    invalid.Mark(bad, FrameScopePhase::CallbackReturned, 105);
    invalid.Mark(bad, static_cast<FrameScopePhase>(255), 105);
    invalid.Mark(bad, FrameScopePhase::JsStarted, 99);
    require(invalid.Snapshot().invalidTransitions == 3 && !invalid.Snapshot().coverageComplete,
        "out-of-order, unknown and backward phases invalidate coverage");
    invalid.Mark(bad, FrameScopePhase::JsStarted, 110);
    invalid.Mark(bad, FrameScopePhase::JsStarted, 120);
    invalid.Release(bad, 150);
    snapshot = invalid.Snapshot();
    require(snapshot.invalidTransitions == 4 && snapshot.longest.phaseMask == 3 && snapshot.longest.acquiredToJsNs == 10,
        "invalid phase changes preserve previous valid state");
    invalid.Mark(9999, FrameScopePhase::JsStarted, 200);
    invalid.Release(9999, 200);
    require(invalid.Snapshot().invalidTransitions == 6, "unknown nonzero tickets are diagnosed");
    invalid.BeginFrame();
    const auto backward = invalid.Acquire(FrameScopeKind::Other, 100);
    invalid.Release(backward, 99);
    snapshot = invalid.Snapshot();
    require(snapshot.released == 1 && snapshot.longest.identity == 0 && snapshot.invalidTransitions == 1,
        "backward release removes active ticket without unsigned duration underflow");
    invalid.BeginFrame();
    require(invalid.Snapshot().coverageComplete, "backward-release refusal still permits a later quiescent reset");
    require(invalid.Acquire(static_cast<FrameScopeKind>(255), 0) == 0 && invalid.Snapshot().invalidTransitions == 1,
        "unknown scope kind is refused without storing an entry");

    FrameScopeTracker bounded;
    std::array<uint64_t, FrameScopeTracker::Capacity> identities{};
    for (size_t i = 0; i < identities.size(); ++i) identities[i] = bounded.Acquire(FrameScopeKind::Other, i);
    bool distinct = identities.front() != 0;
    for (size_t i = 1; i < identities.size(); ++i) distinct = distinct && identities[i] > identities[i - 1];
    require(distinct, "all256 fixed slots produce distinct monotonic nonzero identities");
    require(bounded.Acquire(FrameScopeKind::Other, 300) == 0, "257th simultaneous scope refuses bounded observation");
    snapshot = bounded.Snapshot();
    require(snapshot.acquired == 257 && snapshot.overflow == 1 && !snapshot.coverageComplete && snapshot.released == 0,
        "overflow remains explicit and never changes owner scope counts");
    for (auto identity : identities) bounded.Release(identity, 400);
    require(bounded.Snapshot().released == 256 && bounded.Snapshot().longest.identity == identities[0] &&
        bounded.Snapshot().longest.ageNs == 400, "bounded release retains oldest longest identity and exact release count");
    bounded.BeginFrame();
    const auto reuse = bounded.Acquire(FrameScopeKind::Other, 0);
    require(reuse > identities.back(), "reusing a slot does not reuse a ticket identity");
    bounded.Release(reuse, 0);
    require(bounded.Snapshot().longest.identity == reuse && bounded.Snapshot().longest.ageNs == 0,
        "first completed zero-age scope is still distinguishable from an empty frame");

    FrameScopeTracker ties;
    const auto first = ties.Acquire(FrameScopeKind::Other, 10), second = ties.Acquire(FrameScopeKind::Other, 20);
    ties.Release(second, 30); ties.Release(first, 20);
    require(ties.Snapshot().longest.identity == first, "equal-age records choose older identity deterministically");
    FrameScopeTracker extremes{std::numeric_limits<uint64_t>::max()};
    const auto finalIdentity = extremes.Acquire(FrameScopeKind::Other, 0);
    extremes.Release(finalIdentity, std::numeric_limits<uint64_t>::max());
    require(finalIdentity == std::numeric_limits<uint64_t>::max() && extremes.Snapshot().longest.ageNs == finalIdentity,
        "last uint64 identity and maximum representable elapsed lifetime are exact");
    require(extremes.Acquire(FrameScopeKind::Other, 0) == 0 && extremes.Snapshot().overflow == 1 && !extremes.Snapshot().coverageComplete,
        "exhausted identities refuse observation rather than wrap or reuse");
    extremes.BeginFrame();
    require(extremes.Acquire(FrameScopeKind::Other, 0) == 0, "frame reset cannot revive exhausted identity sequence");

    std::cout << "FrameScopeDiagnostics: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
