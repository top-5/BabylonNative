#pragma once
#include <cstdint>

namespace Babylon::Graphics
{
    enum class FrameScopeKind : uint8_t { Other, Command, Raf };
    enum class FrameScopePhase : uint8_t { Acquired, JsStarted, CallbackReturned, DeferredQueued, DeferredStarted };

    // Completed scope lifetime observations, not CPU time or GPU completion.
    // Phase durations are meaningful only when both endpoint bits are present.
    // ageNs ends at final owner release, not entry into a deferred callback.
    struct FrameScopeRecord
    {
        uint64_t identity{};
        uint64_t acquiredAtNs{};
        FrameScopeKind kind{FrameScopeKind::Other};
        uint64_t ageNs{}, phaseMask{}, acquiredToJsNs{}, callbackNs{}, deferredWaitNs{}, afterDeferredNs{};
    };

    struct FrameScopeSnapshot
    {
        FrameScopeRecord longest{};
        uint64_t acquired{}, released{}, overflow{}, invalidTransitions{};
        // Complete tracking can contain partial phase sequences after cancellation
        // or an exception. Missing phase bits do not invent a zero-duration stage.
        bool coverageComplete{true};
    };
}
