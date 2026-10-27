#pragma once
#include <cstdint>
#include <Babylon/Graphics/FrameScopeDiagnostics.h>

namespace Babylon::Graphics
{
    // Completed frame-thread wall durations, not CPU cycles or GPU completion.
    // Read/configure on the frame thread only. Default off. No history is retained.
    struct FrameTimings
    {
        bool valid{};
        uint64_t sequence{}, overlayNs{}, finishGateNs{}, beforeRenderNs{}, encoderEndNs{};
        uint64_t framePrepareNs{}, bgfxFrameNs{}, frameAfterNs{}, afterRenderNs{}, startNs{}, viewNs{};
        // The gate includes mutex/CV wait and any mid-frame flush. A long gate is
        // not proof of animation cost: command continuations/JS backlog also hold scopes.
        uint64_t midFrameFlushes{};
        FrameScopeSnapshot scopes{};
    };
}
