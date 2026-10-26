#pragma once

#include <Babylon/Api.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bimg { struct ImageContainer; }
namespace bx { struct AllocatorI; }

namespace Babylon::Plugins::NativeEngine
{
    inline constexpr uint64_t PreparedImageCacheMaxBudgetBytes = 512ULL * 1024 * 1024;
    struct PreparedImageCacheContext
    {
        uint32_t rendererType{};
        bool invertY{}, srgb{}, generateMips{}, originBottomLeft{};
        // Exact capability values in TextureFormat order, not a hash/truncated mask.
        std::vector<uint32_t> formatCapabilities;
    };
    struct PreparedImageCacheStatistics
    {
        bool enabled{};
        uint64_t budgetBytes{}, residentBytes{}, entries{};
        uint64_t lookups{}, hits{}, misses{}, stores{}, insertions{}, evictions{};
        uint64_t oversize{}, bypasses{}, rejected{}, allocationFailures{}, keyComparisons{};
    };

    // Default budget=0: no stored images. Max64 entries and512MiB. Configuring
    // clears stored entries, preserving lifetime counters and already returned clones.
    // residentBytes charges encoded+prepared bytes and fixed metadata/context.
    // The fixed64-pointer index, allocator bookkeeping and caller-owned/upload
    // clones are separate. No stored-entry allocation occurs at budget0.
    bool BABYLON_API ConfigurePreparedImageCache(uint64_t budgetBytes) noexcept;
    PreparedImageCacheStatistics BABYLON_API GetPreparedImageCacheStatistics() noexcept;

    // Inputs must remain immutable for each call; no caller buffer is retained.
    // Every hit compares the exact encoded bytes AND complete context despite a
    // hash match. Returns an independent allocator-owned bimg image ornullptr.
    // The caller's allocator must outlive the clone, including bgfx's final-mip
    // release callback. Cache/allocator failures refuse caching and never throw.
    bimg::ImageContainer* BABYLON_API ClonePreparedImage(const uint8_t* encoded, size_t encodedSize,
        const PreparedImageCacheContext& context, bx::AllocatorI& allocator) noexcept;
    bool BABYLON_API StorePreparedImage(const uint8_t* encoded, size_t encodedSize,
        const PreparedImageCacheContext& context, const bimg::ImageContainer& image) noexcept;
}
