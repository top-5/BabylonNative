#pragma once

#include <Babylon/Api.h>
#include <cstdint>

namespace Babylon::Plugins::NativeEngine
{
    // Completed CPU/API attempts, including exceptional returns. Success means normal
    // completion of the instrumented CPU call, not a completed/validated GPU upload.
    struct ResourceOperationStatistics
    {
        uint64_t calls{};
        uint64_t succeeded{};
        uint64_t failed{};
        uint64_t totalNs{};
        uint64_t maxNs{};
        uint64_t bytes{};
    };

    struct ResourceStatistics
    {
        // Encoded image bytes: bimg parsing plus normalization/orientation conversion.
        ResourceOperationStatistics parseImage;
        // Decoded image bytes entering format conversion, inversion and mip generation.
        ResourceOperationStatistics prepareImage;
        // Prepared mip bytes entering bgfx texture creation/update API submission.
        ResourceOperationStatistics textureSubmit;
        // Nested preparation work; overlaps prepareImage and must not be added to it.
        // Input decoded bytes for orientation flip, each format conversion, and mip generation.
        // Only the LoadTexture worker path, excluding per-frame raw/cube texture preparation.
        ResourceOperationStatistics imageFlip;
        ResourceOperationStatistics formatConvert;
        ResourceOperationStatistics mipGenerate;
        // Opt-in JS-thread CPU/API wall spans. RAF nests native operations, and
        // GetReader invokes a JS flush callback; these are not disjoint CPU time.
        ResourceOperationStatistics commandRead, commandScopeDispatch, commandExecute;
        ResourceOperationStatistics vertexBufferCreate, indexBufferCreate, vertexBufferRecord, rafCallbacks;
        bool commandTimingEnabled{};
    };

    // Process-lifetime completed counters: no reset. Worker tasks may overlap; totals
    // cannot be summed into loader wall time. Each field is loaded atomically with
    // relaxed ordering, so the snapshot is not a coherent transaction while busy.
    ResourceStatistics BABYLON_API GetResourceStatistics() noexcept;
    // Configure before runtime creation. Does not reset lifetime counters.
    void BABYLON_API SetCommandTimingEnabled(bool enabled) noexcept;
}
